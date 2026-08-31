# kernel/hal/backend/storage — LittleFS on the internal partition

The first `eos_storage` backend. It satisfies every non-inline declaration in
`kernel/hal/include/eos_storage.h` on the 960 KB `int` LittleFS partition, and
it is why the Files tab, `/api/settings` and `/api/themes` have somewhere to
read from.

| File | Lines | What |
|---|---|---|
| `eos_storage_idf.c` | 928 | the backend: all 22 `eos_storage_*` functions |
| `test/test_storage.c` | 790 | 276 host checks over the path rules, the pools and the routing |

## What it is

One namespace over two mount points, resolved by the first path component:

| Point | Filesystem | On this board | Removable |
|---|---|---|---|
| `/int` | LittleFS on partition label `int`, 960 KB at 0x310000 | mounted, formatted on first boot | no |
| `/sd` | FAT on a microSD | **declared, never mounted** | yes |

Below the mount table everything is POSIX — `open`, `read`, `lseek`, `fsync`,
`stat`, `rename`, `opendir` — because ESP-IDF's VFS already puts that in front
of LittleFS. That is what lets the same file compile and run on the host, where
`/int` is a directory instead of a partition, and it is why the adversarial
checks below run against the same code the board executes rather than a model
of it.

## The card is not there and this does not pretend otherwise

`boards/waveshare-c6-lcd-13.json` says `sdcard.present false`: the slot is
physically on the module, but the JTAG pin scan can only see outputs, so MISO
is invisible to it by construction and CS is an inference. A guessed pinout is
the exact failure the board registry exists to prevent.

So `/sd` is **declared, routed, and absent**:

| Call | Answer |
|---|---|
| `eos_storage_mounts()` | reports `/sd`, `fs: none`, `mounted: false`, `removable: true` |
| `eos_storage_mount_for("/sd/x")` | the `/sd` mount record — routing works |
| `eos_storage_open/stat/mkdir/remove/rename/opendir` under `/sd` | `EOS_ERR_NODEV`, immediately, no bus touched |
| `eos_storage_mount("/sd")` | `EOS_ERR_NODEV` — the board says there is nothing to mount |
| `eos_storage_unmount("/sd")` | `EOS_OK` — unmounting what was never mounted is not an error |

Nothing in that table blocks and nothing in it hangs on a bus with no card on
it. The day the pins are read, the work is: flip `sdcard.present` in the
profile, regenerate the header, and add an SDSPI mount to `mount_one()`. The
path rules, the routing, the handle pools and the root listing already handle a
second mount — the host suite drives `/sd` through all of them today.

FAT itself is **not implemented**. `mount_one()` answers `EOS_ERR_UNSUPPORTED`
for `EOS_FS_FAT`, which `eos_board.h` defines as "valid call, this backend
cannot do it". Writing an SDSPI mount against pins nobody has measured would be
speculative code that cannot be tested and would be wrong in a way nobody
finds until a card is in the slot.

## Path rules

Every public call starts in `path_split()`, and it is the whole of this file's
defence. Paths reach it from `/api/fs/read?path=...` — off the network, from
whoever is on the WiFi.

| Input | Result |
|---|---|
| `/int/a/b` | mount `int`, remainder `a/b` |
| `/int//a/./b/` | mount `int`, remainder `a/b` — separators collapse, `.` drops |
| `/` | no mount, empty remainder: the pseudo-directory that lists the mounts |
| `/int/..`, `/int/../int/x`, `/../..`, `../x` | `EOS_ERR_ARG` |
| `/int/a\..\b` | `EOS_ERR_ARG` — backslash |
| `/int/a<0x01>b`, `/int/a\n`, `/int/<DEL>` | `EOS_ERR_ARG` — control bytes |
| `int/x` (relative), `""`, `NULL` | `EOS_ERR_ARG` |
| `/etc/passwd`, `/INT/x`, `/inte/x` | `EOS_ERR_NOTFOUND` — no such mount |
| 96 bytes or longer | `EOS_ERR_TOOBIG` — never truncated |
| a component 40 bytes or longer | `EOS_ERR_TOOBIG` |
| `/int/café.txt`, `/int/<invalid utf-8>` | accepted — bytes are the filesystem's business |
| `/int/%2e%2e` | accepted as a filename — decoding is `eos_httpd`'s job, done once |

Four decisions worth stating because the obvious alternative is defensible and
worse:

- **`..` is rejected, not resolved.** Folding `a/../b` into `b` is correct and
  it is one off-by-one from being an escape. Nothing in penguinOS builds a path
  that needs a parent, so refusing costs nothing.
- **Backslash is rejected.** FatFs accepts `\` as a path separator. No card is
  mounted today; the rule is here so that finding the pins is a profile edit
  and not a hole.
- **Over-long is refused, never truncated.** A truncated path names a different
  file, and the caller would hand that name straight back to `remove()`.
- **A percent escape is a filename here.** `eos_httpd_path_of()` decodes once,
  at the edge, and rejects a decoded NUL. Decoding a second time down here is
  how `%252e%252e` gets through a two-stage decoder.

An embedded NUL cannot name anything past itself: C strings end there, and the
suite pins that `"/int/a\0b"` resolves to `a` and never to `b`.

## The pools

| Pool | Size | Bytes | Exhausted |
|---|---|---|---|
| `struct eos_file` | `EOS_MAX_FILES` = 4 | 32 | `eos_storage_open()` returns NULL, `errno` `EOS_ERR_POOL` → HTTP 503 |
| `struct eos_dirh` | `EOS_MAX_DIRS` = 2 | 424 | `eos_storage_opendir()` returns NULL, same code |
| mount table | `EOS_MOUNT_MAX` = 2 | 80 | — |
| the pool mutex | one | 84 | — |
| `s_mnt_n`, `s_lock`, `s_inited` | — | 9 | — |

Read off the target object with `nm --print-size`, and they are the whole of
the 629 `.bss` bytes below — 424 + 84 + 80 + 32 + 9, nothing else. The
directory pool is the expensive one: each handle carries the scan's own system
path so that the per-entry `stat` builds no string it did not already own.

`EOS_MAX_DIRS` is **2, not 4**, and that is the header's number rather than
this backend's. Two directory scans is one fewer than `esp_http_server` has
workers, so three simultaneous `/api/fs/list` requests will hand the third an
`EOS_ERR_POOL`. That is a correct, reported failure rather than a corruption,
and the web contract already has a code for it — but it is a real ceiling, and
raising it costs 212 bytes of `.bss` per extra scan if the Files tab ever turns
out to need it.

`EOS_MAX_FILES` must be at least four or a static file request can lose its
slot to three other tabs and 404 for no reason the owner can see. There is a
`#error` on it in the source: `esp_http_server` runs four workers and
`eos_httpd_cfg_default()` says so.

A handle that did not come from the pool is refused — the pointer is checked
against the array bounds *and* the slot stride, so a pointer one byte into a
slot is `EOS_ERR_ARG`. A double close is `EOS_ERR_STATE` and not a silent
success, because the slot has already been handed back and closing it again
closes whatever the next `open()` put there.

The pool claim is the one thing four HTTP workers race for. It is guarded by a
FreeRTOS mutex built with `xSemaphoreCreateMutexStatic()` into storage this
file already owns — still no allocation — and no media call is ever made while
it is held.

## Allocation

| What | Allocates | Where |
|---|---|---|
| everything in `eos_storage_idf.c` | **no** — fixed pools, caller buffers, a static mutex | — |
| `esp_vfs_littlefs_register()` | yes, once per mount: 1,424 B | IDF |
| `open()` | yes, 648 B per open file | IDF's LittleFS VFS |
| `opendir()` | yes, 324 B per open scan | IDF's LittleFS VFS |

Those three figures are `sizeof` read out of the compiled component with the
project's own sdkconfig, not estimates. The mount is `esp_littlefs_t` 128 +
`lfs_t` 128 + a 4-entry file-descriptor cache 16 + the three buffers
`lfs_mount()` allocates when the config leaves them NULL: a 512 B read cache, a
512 B program cache and a 128 B lookahead. An open file is one
`vfs_littlefs_file_t`, which carries its own 512 B `lfs_buffer` inside it — so
`lfs.c`'s per-file cache allocation never fires. An open scan is one
`vfs_littlefs_dir_t`.

The IDF-side allocations are stated rather than hidden. They are bounded by the
pools above — at most four files and two scans exist at once — so the ceiling
is 4 × 648 + 2 × 324 = **3,240 B of transient heap** on top of the mount's
1,424 B, and the file-descriptor cache inside `esp_littlefs` is reused rather
than reallocated after it has grown to its high-water mark. The alternative is
vendoring littlefs and driving `lfs_file_opencfg()` with caller-supplied
buffers, which buys back those 3.2 KB and costs the VFS, the POSIX surface and
the host build.

The three numbers move if `CONFIG_LITTLEFS_CACHE_SIZE` or
`CONFIG_LITTLEFS_LOOKAHEAD_SIZE` move, so both are pinned at the values above
in `firmware/sdkconfig.defaults` rather than left to a third-party component's
defaults.

## Cost

Measured by building the tree twice — once at the commit before this backend
existed, once with it — and diffing `idf.py size-files` and the two `penguinos.bin`
sizes. Both builds used the same sdkconfig, the same board and the same
toolchain. The linker discards the whole backend until something calls it, so a
build that adds the file without wiring `eos_storage_init()` into `app_main`
costs zero and means nothing; the number below is the wired one.

| Item | Flash | Static RAM |
|---|---|---|
| `eos_storage_idf.c` | 1,058 B | 629 B `.bss`, + 4 B `.tbss` per task |
| `libjoltwallet__littlefs.a` — `lfs.c` 20,384, `esp_littlefs.c` 7,941, `littlefs_esp_part.c` 298, `lfs_config.c` 143 | 28,766 B | 136 B |
| the storage block in `app_main` | 268 B | — |
| newlib's `time` (LittleFS keeps an mtime) | 66 B | — |
| `.rodata` and the POSIX/VFS entry points the linker now has to keep | 5,362 B | — |
| **total image delta** | **+35,520 B** | **+768 B** |

That is 1,572,240 bytes of a 3 MB app partition, up from 1,536,720 — 1.2% of
the partition, and it leaves 50% of the slot free. The static RAM is 629 B of
pools in this file plus 136 B inside `esp_littlefs`, rounded up by three bytes
of alignment.

The 4 bytes of `.tbss` are `eos_storage_errno()`'s thread-local word. It is
**per task, not per image**: two HTTP workers fail at the same time for
different reasons and a shared word hands one of them the other's answer. Four
workers and the frame loop is 20 bytes of task memory, which is the price of
the answer being right.

Runtime heap is 1,424 B held for the life of the mount and at most 3,240 B more
while every handle is in use — the arithmetic is in **Allocation** above, from
`sizeof` on the compiled component. It has **not been measured on hardware**;
`app_main` logs `heap after storage` on the boot line, so the first flash
replaces this with a real figure.

## Blocking

Every `eos_storage_*` call blocks; none is safe from an ISR. That was already
true in the header. What matters on this board is *how*.

**A flash write stops the chip.** Erasing or programming the SPI flash turns
the instruction cache off for the duration, and the ESP32-C6 has one core that
is also running the frame loop, the WiFi stack and the HTTP server. A write is
not slow the way a disk is slow — it is a hole in the whole machine.

| Operation | Cost | Notes |
|---|---|---|
| `read` | microseconds | through the flash cache; no stall |
| `stat`, `readdir` entry | microseconds | one metadata read each |
| `write` (buffered) | microseconds | lands in the 512 B program cache |
| `write` that flushes a 128 B page | ~0.5 ms | one SPI-NOR page program |
| `sync` / `close` after a write | one or more 4 KB sector erases | **tens of ms each**, cache off |
| `mount`, already formatted | tens of ms | boot log prints it |
| `mount`, blank partition (first boot only) | mount fails, `lfs_format()`, mount again | hundreds of ms, not seconds — see below |
| `usage` | walks LittleFS's block allocation | which is why it is a separate call from `mounts()` |

A first boot does **not** erase the partition. `esp_littlefs`'s format path is
`lfs_format()`, which erases the two superblock blocks and writes a superblock
pair; it does not touch the other 238. Erasing 960 KB would be twelve seconds
of a board that looks dead. `eos_storage`'s mount logs the milliseconds it
actually took, so the first flash replaces this estimate with a measurement.

The erase figures are SPI-NOR datasheet ranges (25–100 ms per 4 KB sector,
worst case several hundred), **not measured on this board**. They are the
reason for two rules in the source: `eos_storage_write()` performs exactly one
`write()` of exactly what it was handed and returns — it never loops to satisfy
a large request, because that would hold the frame loop for as long as the
request was big — and the caller is the thing that has to be bounded.
`/api/system` already reports `limits.chunk_max` for this reason.

`readdir` costs one `stat` per non-directory entry, for the size the listing
carries. That is bounded by the page size the web contract already imposes, and
it is why directory listings page.

## Wear

The write pattern is: **whole file, truncate, one sync, close** — which is what
`eos_storage_save()` does and what settings will use. LittleFS is copy-on-write
with `block_cycles` 512 and spreads its metadata across all 240 blocks of the
partition.

The arithmetic that matters: a 512-byte settings file rewritten **once per
second** is 86,400 writes a day, spread over 240 blocks, so roughly 360 erases
per block per day. Against a typical 100,000-cycle endurance that is about 277
days. Rewritten **once per keystroke** it is worse than that by whatever a
person types.

So: settings must be debounced before they reach this file. Nothing here can do
it — a filesystem cannot know that four saves in a second were one edit — and
the number above is recorded so the debounce is a decision rather than an
oversight.

## The dependency

`joltwallet/littlefs` `^1.14` (resolved: **1.22.3**), added to
`firmware/components/eos_kernel/idf_component.yml`. **It is a download**: the
first build after that line fetches it from the ESP component registry into
`firmware/managed_components/joltwallet__littlefs/`. It is the only
non-Espressif code in the image.

LittleFS has never been part of ESP-IDF core. This component carries the
upstream littlefs sources plus the VFS binding, which is what lets this backend
be plain POSIX. It is pinned in `firmware/dependencies.lock` with a component
hash.

## Host build

```sh
cc -std=c99 -Wall -Wextra -Werror -O1 \
   -Ikernel/hal/include -Ikernel/wm/include -Iboards/generated \
   kernel/hal/backend/storage/eos_storage_idf.c \
   kernel/hal/backend/storage/test/test_storage.c -o /tmp/tstor && /tmp/tstor
```

276 checks, 0 failed. Also clean under
`-fsanitize=address,undefined`, which is how it is meant to be run:

```sh
cc -std=c99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
   -Ikernel/hal/include -Ikernel/wm/include -Iboards/generated \
   kernel/hal/backend/storage/eos_storage_idf.c \
   kernel/hal/backend/storage/test/test_storage.c -o /tmp/tstor && /tmp/tstor
```

0.05 s plain, 0.41 s under the sanitisers. The suite builds its own sandbox
under `/tmp/eos-storage-test-<pid>`, points `EOS_STORAGE_HOST_ROOT` at it, and
removes it on the way out. `/int` is that directory; `/sd` is absent exactly as
it is on the board, because the suite links the real
`waveshare-c6-lcd-13.h`.

### The fuzz section

`t_split_fuzz()` is the last named case and it is the one that covers what
nobody enumerated: 200,000 paths assembled from the tokens `path_split()` has a
rule about — `/`, `//`, `..`, `.`, `\`, `int`, `sd`, `%2e%2e`, a control byte,
a high byte, a name past `EOS_NAME_MAX` — behind a prefix that is a real mount
three times in four.

The generator is biased on purpose, and the bias is asserted rather than
assumed. A fuzzer over random *bytes* almost never types `int`, so nearly every
path it builds dies at "no such mount" and the remainder builder — the half
that can actually escape — is never reached. The run prints and checks what it
reached:

```
[fuzz] accepted=15032 mounts=12604 deep=3090 climbs=5656
```

15,032 paths came back `EOS_OK`, 12,604 of them on a real mount, 3,090 with a
multi-component remainder, and 5,656 carried a `..` component. Those four
counts are checks, not decoration: a change that made every path fail early
would still report zero escapes and would be worthless.

Two invariants are checked, and they are two because either alone is blind:

| Invariant | Catches |
|---|---|
| **output**: an `EOS_OK` remainder has no `..` *component*, no `\`, no leading, trailing or doubled `/` | a rule that stops filtering — the remainder leaks a separator into `sys_join()` |
| **input**: a path with a `..` *component* never comes back `EOS_OK` | the *wrong fix* — folding `a/../b` into `b`, which leaves an output that looks perfectly clean |

The output check is deliberately per component and not `strstr(rel, "..")`:
`...bb...` contains `..` and is a legal filename. A filter that gets that
backwards blocks real names and still lets the real escape through.

### Mutants

Run against the suite to check it is not decorative. The last column is what
the fuzz section alone caught, with every named case deleted — which is the
question worth asking of a fuzzer.

| Mutant | Suite | Fuzz alone |
|---|---|---|
| fold `..` away instead of refusing it | 21 failures | **yes** — 935 folded paths |
| drop the backslash rule | 3 failures | **yes** — 2,506 escaped remainders |
| reuse slot 0 instead of reporting a full pool | 16 failures | no — the pool cases catch it |
| let a double close succeed | 2 failures | no — same |

Unresolved symbols this file expects someone else to provide:

| Symbol | Owner |
|---|---|
| `eos_board_get()` | the board component |

## Untested paths

| Path | Why |
|---|---|
| the LittleFS mount, format and `esp_littlefs_info()` | needs a flash. Nothing here has run on hardware |
| `EOS_FS_FAT` anywhere | not implemented; no board declares working card pins |
| `EOS_ERR_UNSUPPORTED` from a cross-mount rename | needs two mounted mounts; `/sd` answers `NODEV` first |
| `ENOSPC` on a full partition | maps to `EOS_ERR_IO`; not reachable on a host filesystem with room |
| the pool mutex under real contention | the host suite is single-threaded; four HTTP workers are not |
| `mtime` | `CONFIG_LITTLEFS_USE_MTIME` is on, so LittleFS does keep one and `eos_storage_stat()` returns it. Untested because nothing has run on flash, and because before SNTP the board's clock is 1970 — the first file written on a fresh board carries a 1970 timestamp forever |
