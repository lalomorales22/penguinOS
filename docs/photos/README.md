# Board photos

Two per board, referenced from the gallery in the top-level `README.md`.

| File | What it should show |
|---|---|
| `<profile-id>-1.jpg` | The board running penguinOS — the desktop, the clock, the buddy. Whatever makes it recognisable on a shelf. |
| `<profile-id>-2.jpg` | Detail: the back, the pinout, the microSD slot, the case it lives in. |

`<profile-id>` is the filename stem of the board's `boards/*.json`, so the
photos sort next to the profile they belong to and nothing has to keep a second
mapping in step.

Keep them under about 400 KB each. GitHub renders them at a few hundred pixels
wide in the README table and a 4 MB photo helps nobody.

The README's gallery already has an entry for every verified board with the
filename it expects. Drop a file in with that name and it appears; leave it out
and GitHub shows the alt text, which names the board. Nothing needs editing to
add one.
