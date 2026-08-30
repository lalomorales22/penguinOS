#!/usr/bin/env python3
"""Regenerates design/preview.html from the LIVE kernel.

The preview is not a mockup. It compiles dump_layout.c against the real
eos_wm / eos_bar / eos_theme, runs every board geometry through them for
every theme on disk, and injects that output into preview.tmpl.html. So if
the tiling rule, the bar fitter or a theme changes, rerun this and the page
tells the truth again.

This whole directory is a design artifact. Nothing here is ever flashed and
nothing here is served from the board - it links Google Fonts, which a board
serving pages offline cannot reach. web/ is the thing that ships.
"""
import json, re, subprocess, sys, pathlib

D    = pathlib.Path(__file__).resolve().parent
ROOT = D.parent
BIN  = D / ".dump"

INC = ["wm", "shell", "theme", "hal"]
SRC = ["kernel/wm/eos_wm.c", "kernel/shell/eos_bar.c", "kernel/theme/eos_theme.c"]

def main():
    cc = ["cc", "-std=c99", "-O1", "-o", str(BIN), str(D / "dump_layout.c")]
    cc += [str(ROOT / s) for s in SRC]
    cc += [f"-I{ROOT}/kernel/{i}/include" for i in INC] + ["-lm"]
    if subprocess.run(cc).returncode:
        sys.exit("compile failed - the kernel API may have moved under this tool")

    layouts = json.loads(subprocess.run([str(BIN)], cwd=ROOT,
                                        capture_output=True, text=True, check=True).stdout)

    themes = {}
    for p in sorted((ROOT / "kernel/theme/themes").glob("*.json")):
        # the kernel parser tolerates // comments; json does not
        d = json.loads(re.sub(r"^\s*//.*$", "", p.read_text(), flags=re.M))
        themes[d["name"]] = {k: d[k] for k in ("colors", "ansi", "metrics")}

    missing = [t for t in themes if t not in layouts]
    if missing:
        sys.exit(f"dump_layout.c does not know about {missing} - add them to its THEMES[]")

    data = json.dumps({"themes": themes, "layouts": layouts}, separators=(",", ":"))
    tmpl = (D / "preview.tmpl.html").read_text()
    if "/*__DATA__*/" not in tmpl:
        sys.exit("preview.tmpl.html has lost its /*__DATA__*/ placeholder")
    out = D / "preview.html"
    out.write_text(tmpl.replace("/*__DATA__*/", data))
    BIN.unlink(missing_ok=True)
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size} bytes, "
          f"{len(themes)} themes x {len(layouts[next(iter(layouts))])} panels")

if __name__ == "__main__":
    main()
