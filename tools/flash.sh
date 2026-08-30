#!/usr/bin/env bash
#
# flash.sh - plug in any board in boards/, run this, get ESP-OS.
#
# The flow is: identify the chip with esptool, narrow to profiles in the
# registry, resolve whatever esptool could not (by cache, by the human, or by
# flashing the panel prober and letting the human read the screen), remember the
# answer against the board's MAC, then build and write the real image.
#
# The one non-obvious constraint: two pairs of boards in the registry are
# indistinguishable to esptool - the 3.5in and 4.0in ILI9488 boards differ only
# in a clock rate, and the two ESP32-C5 boards differ only in what is soldered
# to them - so this script must never resolve an ambiguity on its own. --yes is
# permission to write an image, not permission to guess which board it is for.
#
# macOS bash 3.2: no associative arrays, no mapfile, no ${var^^}, no local -n.

set -euo pipefail

TOOLS="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$TOOLS")"
DETECT="$TOOLS/detect.py"
GEN="$TOOLS/gen_board_header.py"
PROBE_DIR="$TOOLS/probe"
BOARDS="$REPO/boards"

PY="${PYTHON:-python3}"

# ------------------------------------------------------------------- options

OPT_PORT=""
OPT_PROFILE=""
OPT_LIST=0
OPT_PROBE=0
OPT_PROVISION_SD=""
OPT_DRY_RUN=0
OPT_YES=0
OPT_ERASE=0
OPT_MONITOR=0
OPT_IDENTIFY=0
OPT_NO_BUILD=0
OPT_NO_NVS=0
OPT_BAUD=""
OPT_BUILD_DIR=""
OPT_PROJECT=""
OPT_SD_PORT=8765

usage() {
    cat <<'EOF'
flash.sh - the ESP-OS universal flasher

usage: tools/flash.sh [options]

  --port PORT          serial device to use (default: the only one attached)
  --profile ID         skip identification and use this board profile
  --list               show attached boards and the whole registry, then stop
  --probe              flash the panel prober and identify the board by eye
  --identify           identify only; write nothing
  --provision-sd [DIR] serve microSD contents over HTTP for the board to pull
                       (default DIR: <repo>/sdcard)
  --dry-run            print every command instead of running the ones that write
  --yes                do not ask before writing (still refuses to guess a board)
  --erase              erase the whole flash first (never implied, never default)
  --monitor            open a serial monitor after flashing
  --baud N             override the upload baud from the profile
  --build-dir DIR      where the built image is (default: <repo>/build/<profile>)
  --project DIR        ESP-IDF project to build (default: autodetected)
  --no-build           do not build; flash whatever is already in the build dir
  --no-nvs             do not stamp the chosen profile into the board's NVS
  --sd-port N          port for --provision-sd (default: 8765)
  -h, --help           this

With no options at all it identifies whatever is plugged in, asks you to confirm
it, then builds and flashes. Nothing is written without --yes or a yes typed at
the prompt, and the prompt always names the port, the profile and the image.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --port)        OPT_PORT="${2:-}"; shift 2 ;;
        --port=*)      OPT_PORT="${1#*=}"; shift ;;
        --profile)     OPT_PROFILE="${2:-}"; shift 2 ;;
        --profile=*)   OPT_PROFILE="${1#*=}"; shift ;;
        --list)        OPT_LIST=1; shift ;;
        --probe)       OPT_PROBE=1; shift ;;
        --identify)    OPT_IDENTIFY=1; shift ;;
        --dry-run)     OPT_DRY_RUN=1; shift ;;
        --yes|-y)      OPT_YES=1; shift ;;
        --erase)       OPT_ERASE=1; shift ;;
        --monitor)     OPT_MONITOR=1; shift ;;
        --no-build)    OPT_NO_BUILD=1; shift ;;
        --no-nvs)      OPT_NO_NVS=1; shift ;;
        --baud)        OPT_BAUD="${2:-}"; shift 2 ;;
        --baud=*)      OPT_BAUD="${1#*=}"; shift ;;
        --build-dir)   OPT_BUILD_DIR="${2:-}"; shift 2 ;;
        --build-dir=*) OPT_BUILD_DIR="${1#*=}"; shift ;;
        --project)     OPT_PROJECT="${2:-}"; shift 2 ;;
        --project=*)   OPT_PROJECT="${1#*=}"; shift ;;
        --sd-port)     OPT_SD_PORT="${2:-}"; shift 2 ;;
        --sd-port=*)   OPT_SD_PORT="${1#*=}"; shift ;;
        --provision-sd)
            OPT_PROVISION_SD="__default__"
            # An optional value: only consume the next argument if it is not
            # itself an option.
            if [ $# -ge 2 ]; then
                case "${2:-}" in
                    -*) : ;;
                    "") : ;;
                    *) OPT_PROVISION_SD="$2"; shift ;;
                esac
            fi
            shift ;;
        --provision-sd=*) OPT_PROVISION_SD="${1#*=}"; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)
            printf 'flash.sh: unknown option %s\n\n' "$1" >&2
            usage >&2
            exit 2 ;;
    esac
done

# --------------------------------------------------------------------- output

say()  { printf '%s\n' "$*"; }
note() { printf '  %s\n' "$*"; }
warn() { printf 'flash.sh: %s\n' "$*" >&2; }
die()  { printf 'flash.sh: %s\n' "$*" >&2; exit 1; }

rule() { printf '%s\n' "--------------------------------------------------------------"; }

head1() { printf '\n%s\n' "$*"; rule; }

# Every command that writes to the board or the filesystem goes through this, so
# --dry-run is one decision made in one place rather than an if per call site.
run() {
    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        printf '  would run:'
        printf ' %s' "$@"
        printf '\n'
        return 0
    fi
    "$@"
}

# ---------------------------------------------------------------- prereqs

command -v "$PY" >/dev/null 2>&1 || die "python3 not found. It is required; set \$PYTHON to point at it."
[ -f "$DETECT" ] || die "missing $DETECT"
[ -d "$BOARDS" ] || die "missing board registry at $BOARDS"

# ------------------------------------------------------- detection defaults
#
# set -u means every variable the eval below might not set has to exist first.
# detect.py emits all of these on a good run and most of them on a bad one; the
# defaults are what makes a bad run degrade instead of abort.

EOS_DETECT_STATUS="unknown"
EOS_DETECT_MESSAGE=""
EOS_BOARDS_DIR="$BOARDS"
EOS_CACHE_PATH=""
EOS_DETECT_BAUD="115200"
EOS_PORTS=""
EOS_PORT_COUNT=0
EOS_ESPTOOL=""
EOS_ESPTOOL_VERSION=""
EOS_ESPTOOL_STYLE="hyphen"
EOS_PROFILE_IDS=""
EOS_PORT=""
EOS_USB_BRIDGE=""
EOS_USB_VID=""
EOS_USB_PID=""
EOS_USB_SERIAL=""
EOS_USB_LOCATION=""
EOS_CHIP_DESC=""
EOS_CHIP_REV=""
EOS_CHIP_TARGET=""
EOS_FLASH_MB=0
EOS_PSRAM=0
EOS_PSRAM_CERTAIN=0
EOS_MAC=""
EOS_PROBE_ERROR=""
EOS_DECISION="unknown"
EOS_DECISION_REASON=""
EOS_PROFILE=""
EOS_CACHED_PROFILE=""
EOS_NEEDS_CONFIRM=0
EOS_NEEDS_PROBE=0
EOS_CANDIDATES=""
EOS_CANDIDATE_COUNT=0
EOS_CANDIDATES_CLEAN=""
EOS_CANDIDATE_CLEAN_COUNT=0
EOS_SAFE_UPLOAD_BAUD=115200
EOS_PROBE_FQBN=""
EOS_CONFIRM_PROMPT=""
EOS_UPLOAD_BAUD=115200
EOS_MONITOR_BAUD=115200
EOS_FLASH_MODE="dio"
EOS_FLASH_FREQ=40
EOS_PROFILE_TARGET=""
EOS_PROFILE_FQBN=""
EOS_PROFILE_PATH=""
EOS_PROFILE_TIER=-1
EOS_BAD_BAUDS=""
EOS_PROFILE_NAME=""

DETECT_JSON=""

detect_args() {
    printf '%s' "--boards $BOARDS"
}

# Probe once, keep the JSON, then flatten the same JSON for the shell. The board
# is only reset once this way, which matters: every esptool connection yanks
# DTR/RTS and reboots the board.
detect_board() {
    local extra=""
    [ -n "$OPT_PORT" ] && extra="--port $OPT_PORT"

    say "Identifying the board..."
    set +e
    # shellcheck disable=SC2086
    DETECT_JSON="$("$PY" "$DETECT" --json --boards "$BOARDS" $extra 2>/dev/null)"
    local rc=$?
    set -e
    if [ $rc -ne 0 ] || [ -z "$DETECT_JSON" ]; then
        # detect.py already printed its own one-line reason to stderr when it
        # could. Re-run it visibly so the user sees that reason.
        # shellcheck disable=SC2086
        "$PY" "$DETECT" --boards "$BOARDS" $extra >&2 || true
        die "board detection failed"
    fi

    local shellvars
    # shellcheck disable=SC2086
    shellvars="$(printf '%s' "$DETECT_JSON" | "$PY" "$DETECT" --shell --from-json - $extra)"
    eval "$shellvars"
}

# --------------------------------------------------------------------- --list

do_list() {
    "$PY" "$DETECT" --boards "$BOARDS" ${OPT_PORT:+--port "$OPT_PORT"} || true
    head1 "cache"
    "$PY" "$DETECT" --boards "$BOARDS" --cache-list || true
    head1 "toolchain"
    report_tool "python3"    "$PY"
    report_tool "esptool"    esptool
    report_tool "arduino-cli" arduino-cli
    report_tool "idf.py"     idf.py
    if ! command -v idf.py >/dev/null 2>&1; then
        note "  ESP-IDF is not on PATH. Source it with . \$IDF_PATH/export.sh"
        note "  before flashing a real image; --probe and --identify do not need it."
    fi
}

report_tool() {
    local label="$1" cmd="$2" where
    if where="$(command -v "$cmd" 2>/dev/null)"; then
        printf '  %-12s %s\n' "$label" "$where"
    else
        printf '  %-12s not found\n' "$label"
    fi
}

# ------------------------------------------------------------- profile choice

profile_field() {
    # profile_field ID KEY - one flashing fact straight from the registry.
    "$PY" "$DETECT" --boards "$BOARDS" --profile "$1" --json \
        | "$PY" -c 'import json,sys; print(json.load(sys.stdin).get(sys.argv[1]) or "")' "$2"
}

load_profile_vars() {
    # Re-read the flashing facts for a profile chosen after detection.
    local vars
    vars="$("$PY" "$DETECT" --boards "$BOARDS" --profile "$1" --shell)"
    eval "$vars"
}

list_contains() {
    # list_contains "a b c" b   - bash 3.2 has no associative arrays.
    local needle="$2" item
    for item in $1; do
        [ "$item" = "$needle" ] && return 0
    done
    return 1
}

ask() {
    # ask "question" - yes/no, no default. Refuses to assume in a pipe.
    local reply
    if [ "$OPT_YES" -eq 1 ]; then
        say "$1 (--yes)"
        return 0
    fi
    if [ ! -t 0 ]; then
        warn "$1"
        warn "nothing to read the answer from (not a terminal) and --yes was not given"
        return 1
    fi
    printf '%s [y/N] ' "$1"
    read -r reply || return 1
    case "$reply" in
        y|Y|yes|YES|Yes) return 0 ;;
        *) return 1 ;;
    esac
}

choose_profile() {
    # Called when detection could not decide. Never decides on its own.
    CHOSEN_BY_HUMAN=1
    local list="$EOS_CANDIDATES_CLEAN"
    [ -z "$list" ] && list="$EOS_CANDIDATES"
    [ -z "$list" ] && list="$EOS_PROFILE_IDS"

    head1 "This board cannot be identified from the outside"
    say "$EOS_DECISION_REASON"
    say ""
    say "esptool sees: ${EOS_CHIP_DESC:-?} ${EOS_CHIP_REV:+rev $EOS_CHIP_REV}, \
${EOS_FLASH_MB}MB flash, MAC ${EOS_MAC:-?}"
    say "That is everything it can see. What is on the SPI bus is not readable;"
    say "boards/README.md records why. So this is your call:"
    say ""

    local i=1 id
    for id in $list; do
        printf '  %d) %-22s %s\n' "$i" "$id" "$(profile_field "$id" name)"
        local prompt
        prompt="$(profile_field "$id" confirm_prompt)"
        [ -n "$prompt" ] && printf '     %s\n' "$prompt"
        i=$((i + 1))
    done
    say ""
    say "  p) flash the panel prober and decide by looking at the screen"
    say "  q) quit"
    say ""

    if [ ! -t 0 ]; then
        warn "cannot ask which board this is: not a terminal."
        warn "Re-run with --profile <id>, or with --probe to decide by eye."
        exit 1
    fi

    local reply
    printf 'which board is this? '
    read -r reply || exit 1
    case "$reply" in
        q|Q) say "nothing written."; exit 0 ;;
        p|P) run_prober; return ;;
        ''|*[!0-9]*)
            if list_contains "$list" "$reply"; then
                CHOSEN="$reply"
            else
                die "not one of the choices: $reply"
            fi ;;
        *)
            local n=1
            CHOSEN=""
            for id in $list; do
                [ "$n" -eq "$reply" ] && CHOSEN="$id"
                n=$((n + 1))
            done
            [ -n "$CHOSEN" ] || die "not one of the choices: $reply" ;;
    esac
}

# ------------------------------------------------------------------- prober

run_prober() {
    CHOSEN_BY_HUMAN=1
    local sketch="$PROBE_DIR/probe.ino"
    [ -f "$sketch" ] || die "missing $sketch"
    command -v arduino-cli >/dev/null 2>&1 || die \
        "arduino-cli not found, and the panel prober is an Arduino sketch.
  brew install arduino-cli && arduino-cli core install esp32:esp32
  Or pick the board yourself with --profile <id>."

    local fqbn="${EOS_PROBE_FQBN:-}"
    [ -n "$fqbn" ] || fqbn="$(fqbn_for_target "$EOS_CHIP_TARGET")"
    [ -n "$fqbn" ] || die "no arduino-cli FQBN known for chip target '$EOS_CHIP_TARGET'"

    local baud="${OPT_BAUD:-$EOS_SAFE_UPLOAD_BAUD}"

    head1 "Panel prober"
    say "The prober walks every panel configuration this chip could be wired to"
    say "and draws a labelled test card under each. A wrong driver is never"
    say "subtle - the wrong pixel format tears visibly - so the pass that renders"
    say "a clean card names the board."
    say ""
    note "port      $EOS_PORT"
    note "fqbn      $fqbn"
    note "baud      $baud  (the lowest every candidate is known to survive)"
    note "sketch    $sketch"
    say ""
    say "This overwrites whatever is on the board. The real image goes on after."
    ask "flash the prober?" || { say "nothing written."; exit 0; }

    run arduino-cli compile --fqbn "$fqbn" "$PROBE_DIR" \
        || die "the prober did not compile.
  It needs the 'GFX Library for Arduino' library:
    arduino-cli lib install \"GFX Library for Arduino\""
    run arduino-cli upload --fqbn "${fqbn}:UploadSpeed=${baud}" \
        -p "$EOS_PORT" "$PROBE_DIR" \
        || die "the prober did not upload. See the troubleshooting table in tools/README.md."

    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        say ""
        say "  (dry run: stopping here, before the part that needs your eyes)"
        return
    fi

    say ""
    say "Watch the screen. Each pass holds for about 10 seconds and prints its"
    say "name on the panel and on serial. Open the monitor in another terminal:"
    say ""
    note "arduino-cli monitor -p $EOS_PORT -c baudrate=115200"
    say ""
    say "Note the pass whose test card was clean - readable label, four clean"
    say "colour bars, a smooth grey ramp and an unbroken stripe field. Then come"
    say "back here."
    say ""

    local list="$EOS_CANDIDATES_CLEAN"
    [ -z "$list" ] && list="$EOS_CANDIDATES"
    local i=1 id
    for id in $list; do
        printf '  %d) %s\n' "$i" "$id"
        i=$((i + 1))
    done
    printf '  q) none of them rendered\n\n'

    if [ ! -t 0 ]; then
        warn "not a terminal, so the answer cannot be read here."
        warn "Re-run with --profile <id> once you know which pass was clean."
        exit 1
    fi

    local reply
    printf 'which pass rendered correctly? '
    read -r reply || exit 1
    case "$reply" in
        q|Q)
            say ""
            say "Then the panel is not one of these. Check the backlight pin first -"
            say "a panel with no backlight is black under every driver. See the"
            say "troubleshooting table in tools/README.md."
            exit 1 ;;
        ''|*[!0-9]*) die "not one of the choices: $reply" ;;
        *)
            local n=1
            CHOSEN=""
            for id in $list; do
                [ "$n" -eq "$reply" ] && CHOSEN="$id"
                n=$((n + 1))
            done
            [ -n "$CHOSEN" ] || die "not one of the choices: $reply" ;;
    esac
    say "-> $CHOSEN"
}

fqbn_for_target() {
    case "$1" in
        esp32)   printf 'esp32:esp32:esp32' ;;
        esp32c5) printf 'esp32:esp32:esp32c5' ;;
        esp32s3) printf 'esp32:esp32:esp32s3' ;;
        *)       printf '' ;;
    esac
}

# --------------------------------------------------------------------- build

find_project() {
    if [ -n "$OPT_PROJECT" ]; then
        printf '%s' "$OPT_PROJECT"
        return
    fi
    local d
    for d in "$REPO" "$REPO/firmware" "$REPO/app"; do
        if [ -f "$d/CMakeLists.txt" ]; then
            printf '%s' "$d"
            return
        fi
    done
    printf ''
}

build_dir_for() {
    if [ -n "$OPT_BUILD_DIR" ]; then
        printf '%s' "$OPT_BUILD_DIR"
    else
        printf '%s' "$REPO/build/$1"
    fi
}

generate_header() {
    local profile="$1"
    [ -f "$GEN" ] || { warn "no $GEN, skipping header generation"; return 0; }
    local out="$BOARDS/generated/$profile.h"
    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        say "  checking the profile (dry run does not write the header)"
        "$PY" "$GEN" --check "$BOARDS/$profile.json" || die "$profile.json does not validate"
        printf '  would run: %s %s %s %s\n' "$PY" "$GEN" "$BOARDS/$profile.json" "$out"
        return 0
    fi
    run mkdir -p "$BOARDS/generated"
    run "$PY" "$GEN" "$BOARDS/$profile.json" "$out" \
        || die "could not generate the board header for $profile"
    say "  header    $out"
}

build_image() {
    local profile="$1" bdir="$2" project
    project="$(find_project)"

    if [ "$OPT_NO_BUILD" -eq 1 ]; then
        say "  build     skipped (--no-build)"
        return 0
    fi
    if [ -z "$project" ]; then
        say "  build     no ESP-IDF project found"
        note "Looked for CMakeLists.txt in $REPO, $REPO/firmware and $REPO/app."
        note "Point at it with --project DIR, or use --no-build to flash a"
        note "prebuilt image already in $bdir."
        return 1
    fi
    if ! command -v idf.py >/dev/null 2>&1; then
        say "  build     idf.py is not on PATH"
        note "Source ESP-IDF first:  . \$IDF_PATH/export.sh"
        note "Or use --no-build to flash a prebuilt image already in $bdir."
        return 1
    fi

    say "  project   $project"
    say "  build dir $bdir"
    run "$PY" -c 'import os,sys; os.makedirs(sys.argv[1], exist_ok=True)' "$bdir"
    run idf.py -C "$project" -B "$bdir" \
        -D EOS_BOARD_PROFILE="$profile" \
        set-target "$EOS_PROFILE_TARGET" \
        || die "idf.py set-target failed"
    run idf.py -C "$project" -B "$bdir" -D EOS_BOARD_PROFILE="$profile" build \
        || die "the build failed"
    return 0
}

# ---------------------------------------------------------------- flash plan
#
# idf.py writes flasher_args.json next to the image: the offsets, the files and
# the exact write_flash arguments the build was configured for. Reading it is
# how this script flashes with plain esptool without duplicating any of the
# build's decisions.

flash_plan() {
    local bdir="$1"
    "$PY" - "$bdir" <<'PYEOF'
import json, os, sys

bdir = sys.argv[1]
path = os.path.join(bdir, "flasher_args.json")
try:
    with open(path) as fh:
        args = json.load(fh)
except Exception as exc:
    sys.stderr.write("cannot read %s: %s\n" % (path, exc))
    sys.exit(1)

extra = args.get("extra_esptool_args") or {}
print("CHIP\t%s" % (extra.get("chip") or ""))
print("ARGS\t%s" % " ".join(args.get("write_flash_args") or []))

files = args.get("flash_files") or {}
missing = []
for offset in sorted(files, key=lambda o: int(o, 0)):
    rel = files[offset]
    if not rel:
        continue
    full = os.path.join(bdir, rel)
    if not os.path.exists(full):
        missing.append(full)
    print("FILE\t%s\t%s" % (offset, full))
for m in missing:
    print("MISSING\t%s" % m)
PYEOF
}

esptool_sub() {
    # esptool 5 renamed every subcommand to hyphens; 4 only knows underscores.
    if [ "$EOS_ESPTOOL_STYLE" = "hyphen" ]; then
        printf '%s' "$1"
    else
        printf '%s' "$(printf '%s' "$1" | tr '-' '_')"
    fi
}

# ------------------------------------------------------------------ the write

write_image() {
    local profile="$1" bdir="$2" baud="$3"
    local plan chip args
    plan="$(flash_plan "$bdir")" || die \
        "no flasher_args.json in $bdir - nothing has been built for $profile yet."

    if printf '%s' "$plan" | grep -q '^MISSING'; then
        printf '%s\n' "$plan" | sed -n 's/^MISSING\t/  missing: /p' >&2
        die "the build directory is incomplete; rebuild before flashing"
    fi

    chip="$(printf '%s\n' "$plan" | sed -n 's/^CHIP\t//p')"
    args="$(printf '%s\n' "$plan" | sed -n 's/^ARGS\t//p')"

    # Two things at once: the argument list esptool gets, and the human-readable
    # table. Built in one pass so they can never disagree.
    local files="" shown="" offset file line
    while IFS= read -r line; do
        case "$line" in
            FILE*)
                offset="$(printf '%s' "$line" | cut -f2)"
                file="$(printf '%s' "$line" | cut -f3)"
                files="$files $offset $file"
                shown="$shown$(printf '    %-9s %s' "$offset" "$file")
"
                ;;
        esac
    done <<EOF
$plan
EOF

    head1 "About to write"
    note "port      $EOS_PORT"
    note "board     $profile  ${EOS_PROFILE_NAME:-}"
    note "chip      ${EOS_CHIP_DESC:-?}  mac ${EOS_MAC:-?}"
    note "baud      $baud"
    [ -n "$EOS_BAD_BAUDS" ] && note "known bad $EOS_BAD_BAUDS  (this profile fails at these)"
    note "build dir $bdir"
    say  ""
    say  "  images:"
    printf '%s' "$shown"
    [ "$OPT_ERASE" -eq 1 ] && { say ""; say "  the whole flash will be ERASED first (--erase)"; }
    say ""

    ask "write this to $EOS_PORT?" || { say "nothing written."; exit 0; }

    local esptool_bin="${EOS_ESPTOOL:-esptool}"

    if [ "$OPT_ERASE" -eq 1 ]; then
        # shellcheck disable=SC2086
        run $esptool_bin --port "$EOS_PORT" --baud "$baud" \
            "$(esptool_sub erase-flash)" || die "erase failed"
    fi

    # shellcheck disable=SC2086
    run $esptool_bin ${chip:+--chip "$chip"} --port "$EOS_PORT" --baud "$baud" \
        "$(esptool_sub write-flash)" $args $files \
        || die "flashing failed. See the troubleshooting table in tools/README.md."
}

# ------------------------------------------------------------ the NVS stamp
#
# The profile is compiled into the image, so the firmware does not need this.
# What it buys is that a later run of this script can read back which profile a
# board was flashed with, without asking the human again.

nvs_region() {
    # nvs_region BUILD_DIR -> "offset size", read from the built partition table.
    "$PY" - "$1" <<'PYEOF'
import os, struct, sys

bdir = sys.argv[1]
path = os.path.join(bdir, "partition_table", "partition-table.bin")
if not os.path.exists(path):
    sys.exit(1)
with open(path, "rb") as fh:
    blob = fh.read()

# esp_partition_info_t: magic u16 (0x50AA), type u8, subtype u8, offset u32,
# size u32, label[16], flags u32 - 32 bytes per entry.
for i in range(0, len(blob) - 32 + 1, 32):
    entry = blob[i:i + 32]
    magic, ptype, subtype, offset, size = struct.unpack("<HBBII", entry[:12])
    if magic != 0x50AA:
        break
    label = entry[12:28].split(b"\x00")[0].decode("ascii", "replace")
    # type 1 = data, subtype 2 = nvs
    if ptype == 1 and subtype == 2 and label == "nvs":
        print("0x%x %d" % (offset, size))
        sys.exit(0)
sys.exit(1)
PYEOF
}

find_nvs_gen() {
    local p
    for p in \
        "${IDF_PATH:-}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    do
        [ -n "${IDF_PATH:-}" ] && [ -f "$p" ] && { printf '%s' "$p"; return 0; }
    done
    return 1
}

stamp_nvs() {
    local profile="$1" bdir="$2" baud="$3"
    [ "$OPT_NO_NVS" -eq 1 ] && return 0

    local region offset size gen
    if ! region="$(nvs_region "$bdir" 2>/dev/null)"; then
        say "  nvs       skipped: no nvs partition in the built partition table"
        return 0
    fi
    offset="${region%% *}"
    size="${region##* }"

    if ! gen="$(find_nvs_gen)"; then
        say "  nvs       skipped: nvs_partition_gen.py not found (needs \$IDF_PATH)"
        note "The MAC cache still records this board; only the on-board stamp is missing."
        return 0
    fi

    local tmp csv bin
    tmp="$(mktemp -d)" || return 0
    csv="$tmp/eos_nvs.csv"
    bin="$tmp/eos_nvs.bin"
    cat >"$csv" <<EOF
key,type,encoding,value
eos,namespace,,
board_id,data,string,$profile
board_mac,data,string,${EOS_MAC:-unknown}
EOF

    say "  nvs       stamping $profile at $offset ($size bytes)"
    if ! run "$PY" "$gen" generate "$csv" "$bin" "$size" >/dev/null 2>&1; then
        say "  nvs       skipped: nvs_partition_gen.py could not build the image"
        rm -rf "$tmp"
        return 0
    fi
    local esptool_bin="${EOS_ESPTOOL:-esptool}"
    # shellcheck disable=SC2086
    run $esptool_bin --port "$EOS_PORT" --baud "$baud" \
        "$(esptool_sub write-flash)" "$offset" "$bin" >/dev/null \
        || say "  nvs       stamp failed; the local MAC cache still has the answer"
    rm -rf "$tmp"
}

read_nvs_stamp() {
    # Best effort readback: dump the nvs partition and look for a profile id in
    # it. NVS stores short string entries as plain ASCII, so a scan is enough to
    # answer "which of these five ids is stamped here" without a full parser.
    local bdir="$1" baud="$2"
    local region offset size
    region="$(nvs_region "$bdir" 2>/dev/null)" || return 1
    offset="${region%% *}"
    size="${region##* }"
    local tmp
    tmp="$(mktemp -d)" || return 1
    local esptool_bin="${EOS_ESPTOOL:-esptool}"
    if $esptool_bin --port "$EOS_PORT" --baud "$baud" \
        "$(esptool_sub read-flash)" "$offset" "$size" "$tmp/nvs.bin" >/dev/null 2>&1; then
        local id
        for id in $EOS_PROFILE_IDS; do
            if LC_ALL=C grep -aq -- "$id" "$tmp/nvs.bin" 2>/dev/null; then
                printf '%s' "$id"
                rm -rf "$tmp"
                return 0
            fi
        done
    fi
    rm -rf "$tmp"
    return 1
}

# ------------------------------------------------------------- sd provisioning

lan_address() {
    local ip iface
    for iface in en0 en1 en2 en3 en4 en5 eth0 wlan0; do
        if ip="$(ipconfig getifaddr "$iface" 2>/dev/null)" && [ -n "$ip" ]; then
            printf '%s' "$ip"
            return 0
        fi
    done
    # Linux, and macOS with ipconfig missing.
    if command -v hostname >/dev/null 2>&1; then
        ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
        [ -n "$ip" ] && { printf '%s' "$ip"; return 0; }
    fi
    printf ''
}

provision_sd() {
    local dir="$1"
    [ "$dir" = "__default__" ] && dir="$REPO/sdcard"
    if [ ! -d "$dir" ]; then
        die "no such directory: $dir
  --provision-sd serves a directory of microSD contents (themes, buddy, the web
  app) over HTTP so the board can pull them down. Create it, or pass the path:
    tools/flash.sh --provision-sd path/to/sdcard"
    fi

    local ip
    ip="$(lan_address)"
    [ -n "$ip" ] || ip="<this-mac's-ip>"

    head1 "microSD provisioning"
    note "serving   $dir"
    note "url       http://$ip:$OPT_SD_PORT/"
    say ""
    say "On the board, point its provisioning mode at that URL - on the cyd24"
    say "firmware that is:"
    say ""
    note "/sdload http://$ip:$OPT_SD_PORT/"
    say ""
    say "The board reboots into a WiFi-only mode, pulls the tree, and returns."
    say "Add files here and run this again any time. Ctrl-C stops the server."
    say ""

    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        printf '  would run: %s -m http.server %s --directory %s\n' \
            "$PY" "$OPT_SD_PORT" "$dir"
        return 0
    fi
    "$PY" -m http.server "$OPT_SD_PORT" --directory "$dir"
}

# ---------------------------------------------------------------------- main

if [ "$OPT_LIST" -eq 1 ]; then
    do_list
    exit 0
fi

if [ -n "$OPT_PROVISION_SD" ] && [ "$OPT_PROBE" -eq 0 ] && [ -z "$OPT_PROFILE" ] \
   && [ "$OPT_IDENTIFY" -eq 0 ]; then
    # --provision-sd on its own does not touch the board at all.
    provision_sd "$OPT_PROVISION_SD"
    exit 0
fi

head1 "ESP-OS flasher"
detect_board

case "$EOS_DETECT_STATUS" in
    no-ports|no-boards|no-such-port|no-esptool|multiple-ports|multiple-boards|unreachable)
        say ""
        say "$EOS_DETECT_MESSAGE"
        [ -n "$EOS_PROBE_ERROR" ] && { say ""; note "$EOS_PROBE_ERROR"; }
        if [ "$EOS_PORT_COUNT" -gt 0 ]; then
            say ""
            say "serial devices seen:"
            for p in $EOS_PORTS; do note "$p"; done
        fi
        say ""
        say "Nothing was written. tools/README.md has a table of the failure modes"
        say "this hits most often."
        exit 0
        ;;
esac

say ""
note "port      $EOS_PORT"
note "chip      ${EOS_CHIP_DESC:-?}${EOS_CHIP_REV:+ rev $EOS_CHIP_REV}  ${EOS_FLASH_MB}MB flash"
note "mac       ${EOS_MAC:-?}"
[ -n "$EOS_USB_BRIDGE" ] && note "usb       $EOS_USB_BRIDGE${EOS_USB_LOCATION:+ at $EOS_USB_LOCATION}"
note "decision  $EOS_DECISION - $EOS_DECISION_REASON"

CHOSEN=""
# Set when the profile came from a person rather than from narrowing. The
# registry's confirm_prompt is exactly the question the menu and the prober have
# just asked, so asking it a second time is noise.
CHOSEN_BY_HUMAN=0

if [ -n "$OPT_PROFILE" ]; then
    list_contains "$EOS_PROFILE_IDS" "$OPT_PROFILE" \
        || die "no profile called $OPT_PROFILE. Known: $EOS_PROFILE_IDS"
    CHOSEN="$OPT_PROFILE"
    say "  profile   $CHOSEN (from --profile)"
    if [ -n "$EOS_CANDIDATES" ] && ! list_contains "$EOS_CANDIDATES" "$CHOSEN"; then
        warn "$CHOSEN does not match this chip. Ruled out by detection."
        ask "use it anyway?" || exit 1
    fi
elif [ "$OPT_PROBE" -eq 1 ]; then
    run_prober
elif [ "$EOS_DECISION" = "pinned" ]; then
    CHOSEN="$EOS_PROFILE"
    say "  profile   $CHOSEN (already confirmed for this board)"
elif [ "$EOS_DECISION" = "unique" ]; then
    CHOSEN="$EOS_PROFILE"
    say "  profile   $CHOSEN"
elif [ "$EOS_DECISION" = "none" ]; then
    say ""
    say "This chip matches no profile in the registry. Adding one is the fix -"
    say "boards/README.md has the checklist. Nothing was written."
    exit 1
else
    choose_profile
fi

[ -n "$CHOSEN" ] || die "no board profile chosen; nothing written"

load_profile_vars "$CHOSEN"

# The one-time human confirmation the registry asks for. It is folded into the
# normal pre-write prompt rather than being a second question, but it is not
# skipped: a --yes run has already accepted the profile by asking for it.
if [ "$EOS_NEEDS_CONFIRM" -eq 1 ] && [ -n "$EOS_CONFIRM_PROMPT" ] \
   && [ "$OPT_YES" -eq 0 ] && [ -z "$OPT_PROFILE" ] \
   && [ "$CHOSEN_BY_HUMAN" -eq 0 ]; then
    head1 "Confirm the board"
    say "$EOS_CONFIRM_PROMPT"
    say ""
    ask "is that this board?" || {
        say ""
        say "Then pick it yourself with --profile <id>, or use --probe to decide"
        say "by looking at the screen. Nothing was written."
        exit 0
    }
fi

if [ "$OPT_IDENTIFY" -eq 1 ]; then
    head1 "Identified"
    note "port      $EOS_PORT"
    note "profile   $CHOSEN  ${EOS_PROFILE_NAME:-}"
    note "tier      $EOS_PROFILE_TIER"
    note "target    $EOS_PROFILE_TARGET"
    note "upload    $EOS_UPLOAD_BAUD baud"
    say ""
    say "Nothing was written (--identify)."
    exit 0
fi

# Remember the answer before writing anything: if the flash fails halfway, the
# identification was still work the human did and should not have to redo.
if [ -n "$EOS_MAC" ] && [ "$EOS_DECISION" != "pinned" ]; then
    if [ "$OPT_DRY_RUN" -eq 1 ]; then
        say "  cache     would remember $EOS_MAC as $CHOSEN"
    else
        "$PY" "$DETECT" --boards "$BOARDS" --remember "$EOS_MAC=$CHOSEN" \
            --port "$EOS_PORT" >/dev/null 2>&1 \
            || warn "could not write the MAC cache; carrying on"
        say "  cache     $EOS_MAC -> $CHOSEN"
    fi
fi

BAUD="${OPT_BAUD:-$EOS_UPLOAD_BAUD}"
if [ -n "$EOS_BAD_BAUDS" ] && list_contains "$EOS_BAD_BAUDS" "$BAUD"; then
    warn "$BAUD is on this profile's known-bad list: $EOS_BAD_BAUDS"
    warn "The registry records that it fails with 'Invalid head of packet (0xFF)'."
    ask "use $BAUD anyway?" || die "refusing to use a baud the registry says fails"
fi

BUILD_DIR="$(build_dir_for "$CHOSEN")"

head1 "Build"
generate_header "$CHOSEN"
if ! build_image "$CHOSEN" "$BUILD_DIR"; then
    say ""
    say "Nothing was written to the board."
    exit 1
fi

write_image "$CHOSEN" "$BUILD_DIR" "$BAUD"
stamp_nvs "$CHOSEN" "$BUILD_DIR" "$BAUD"

head1 "Done"
note "$CHOSEN is on $EOS_PORT"
note "monitor with: arduino-cli monitor -p $EOS_PORT -c baudrate=$EOS_MONITOR_BAUD"

if [ -n "$OPT_PROVISION_SD" ]; then
    provision_sd "$OPT_PROVISION_SD"
elif [ "$OPT_MONITOR" -eq 1 ]; then
    if command -v idf.py >/dev/null 2>&1; then
        run idf.py -B "$BUILD_DIR" -p "$EOS_PORT" monitor
    elif command -v arduino-cli >/dev/null 2>&1; then
        run arduino-cli monitor -p "$EOS_PORT" -c "baudrate=$EOS_MONITOR_BAUD"
    else
        warn "no monitor available (neither idf.py nor arduino-cli found)"
    fi
fi
