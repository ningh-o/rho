#!/usr/bin/env python3
"""Embed boot/prelude.rho verbatim into boot/prelude.c.

Deterministic: same input file → byte-identical output. Run from the
repo root after editing the prelude; the checked-in prelude.c is the
canary for accidental drift.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "boot" / "prelude.rho"
DST = ROOT / "boot" / "prelude.c"


def main() -> int:
    src = SRC.read_bytes().decode("utf-8")
    lines = src.split("\n")
    if lines and lines[-1] == "":
        lines.pop()  # keep exactly one trailing newline in the data
    out = [
        "// prelude.c — prelude.rho embedded verbatim (generated form; the",
        "// .rho file is the source of truth — regenerate with",
        "// tools/embed_prelude.py).",
        'const char *prelude_src(void) {',
        "  return",
    ]
    esc = []
    for ln in lines:
        esc.append('    "%s\\n"' % ln.replace("\\", "\\\\").replace('"', '\\"'))
    out.append("\n".join(esc) + ";")
    out.append("      ;")
    out.append("}")
    DST.write_text("\n".join(out) + "\n", encoding="utf-8")
    print("embedded %d lines" % len(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
