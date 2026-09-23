#!/usr/bin/env python3
r"""Sync the behavior-critical prelude tails into libs/compiler/prelude_src.rho.

The mirror embeds its own prelude copies (a released image is standalone —
no boot/ tree, no CWD dependence). Two of the four embedded tails are
behavior-critical and MUST track boot byte for byte (the corpus differential
compares boot-built vs mirror-built program output, and the prelude carries
the whole formatting/allocator machine):

  core.rho   — from boot/prelude/core.rho (tracks every boot change)
  wasi.rho   — from boot/prelude/wasi.rho (the wasm32-wasi tail)

The other two are MIRROR-OWNED native tails — boot cannot emit a native
image at all, so these live only here and this tool never touches them:

  mac.rho    — freestanding arm64-mac tail (runtime stubs ride in the image)
  hosted.rho — libc tail (amd64-linux / hosted runs)

Run from the repo root after editing boot/prelude/{core,wasi}.rho:

    python3 tools/embed-prelude.py

The embedded strings are triple-quoted literals (fully literal semantics;
the boot preludes never contain a raw triple-quote, so no escaping needed).
The generated read_prelude concatenates with `+` — the language surface
since cat retired, which the mirror supports from this round on.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MIRROR = ROOT / "libs" / "compiler" / "prelude_src.rho"

READ_PRELUDE = """pub fn read_prelude(tail: string) -> Result[string, string] {
  if main.streq(tail, "mac.rho") {
    return Result.Ok(prelude_core_src() + prelude_mac_src());
  }
  if main.streq(tail, "wasi.rho") {
    return Result.Ok(prelude_core_src() + prelude_wasi_src());
  }
  if main.streq(tail, "hosted.rho") {
    return Result.Ok(prelude_core_src() + prelude_hosted_src());
  }
  return Result.Ok(prelude_core_src());
}"""


def fn_block(name: str, src: str) -> str:
    # triple-quoted literals decode backslash escapes exactly like
    # single-line strings (boot lex.c), so every literal backslash in the
    # source doubles; raw newlines ride verbatim (that is the point of the
    # triple-quote). The preludes never contain a raw """ — assert it.
    assert '"""' not in src, name
    return 'fn prelude_%s_src() -> string { return """%s"""; }' % (
        name,
        src.replace("\\", "\\\\"),
    )


def main() -> None:
    tracked = {}
    for name in ("core", "wasi"):
        p = ROOT / "boot" / "prelude" / (name + ".rho")
        tracked[name] = p.read_bytes().decode("utf-8")

    text = MIRROR.read_text()
    for name, src in tracked.items():
        pat = re.compile(
            r"fn prelude_%s_src\(\) -> string \{ return \"\"\".*?\"\"\"; \}" % name,
            re.S,
        )
        if not pat.search(text):
            sys.exit("embed-prelude: no prelude_%s_src() in prelude_src.rho" % name)
        # lambda replacement: re.sub escape-processes plain replacement
        # strings (\\n in the embedded body would decode to \n) — a lambda
        # returns it verbatim
        text = pat.sub(lambda m: fn_block(name, src), text, count=1)

    # read_prelude lives at the tail of prelude_src.rho; regenerate it
    # wholesale (the old copy used the retired cat(); the new one spells
    # concatenation `+`)
    if not re.search(r"pub fn read_prelude\(tail: string\) -> Result\[string, string\] \{", text):
        sys.exit("embed-prelude: no read_prelude in prelude_src.rho")
    text = re.sub(
        r"pub fn read_prelude\(tail: string\) -> Result\[string, string\] \{.*?\n\}",
        lambda m: READ_PRELUDE,
        text,
        count=1,
        flags=re.S,
    )

    MIRROR.write_text(text)
    total = sum(len(v) for v in tracked.values())
    print("synced core + wasi (%d bytes); mac/hosted tails left as mirror-owned" % total)


if __name__ == "__main__":
    main()
