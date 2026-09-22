# Bootstrap — the seed chain on one page

The C compiler under `boot/src` is the permanent bootstrap seed. It keeps
only: compile rho → wasm32-wasi, and the corpus oracle. Features are
frozen at 0.4.0 + triple-quote; fmt and the native backends (mac/linux,
arm64(aarch64)/amd64) live in the self-hosted compiler (`self/rho.rho`).
Everything downstream of it is built by rho itself. This page records the
seed chain, who produces which artifact, and how to rebuild each one.

## The chain

```
boot/src (C seed)
  └── build/rho-boot          cc over boot/src/*.c (+ generated prelude_data.c)
        ├── build/gate/m.wasm       boot compiles self/rho.rho --target wasm32-wasi
        │     └── build/gate/child.wasm       m.wasm compiles self/rho.rho
        │           └── build/gate/grandchild.wasm   child compiles self/rho.rho
        └── site/assets/rho.wasm    boot compiles self/rho.rho (the site asset;
              └── build/rho.wasm    a byte-identical copy at the historical
                                    path, for the tools that read it)
```

- **boot** — `build/rho-boot`, plain C, built by `make build/rho-boot`. The
  oracle: frozen corpus goldens (`corpus/*.out`) are its word, and it is
  the only tool allowed to regenerate them. It ships one target:
  `--target wasm32-wasi`; any other target is rejected with a clear error
  (the native backends live in the self-hosted compiler).
- **the mirror** — `self/rho.rho`, the compiler written in rho. Boot turns
  it into `build/gate/m.wasm` (the gate's name for it). The same build is
  the shipped compiler: `make site` produces `site/assets/rho.wasm` with
  `boot build self/rho.rho --target wasm32-wasi`, and `build/rho.wasm` is
  kept as a copy of that artifact for the tools that read the historical
  path (the site tests, the LSP, the vite plugin). No wasi-sdk anywhere:
  rho emits its own wasm, in-process.
- **child** — the mirror compiling itself: `m.wasm` builds
  `build/gate/child.wasm`.
- **grandchild** — the child compiling itself: `child.wasm` builds
  `build/gate/grandchild.wasm`. Stage 2 == stage 3 is never assumed
  byte-for-byte (two correct compilers may lay out functions
  differently); the grandchild is graded on behavior — it must rebuild
  every corpus program to the same stdout + exit code as boot.

## The feature subset law

`self/rho.rho` may only use language features boot already implements.
This is not a convention — `tools/gate.sh` enforces it: the **build-mirror**
leg is boot compiling the mirror, so a mirror source that runs ahead of the
seed simply fails to build and the gate goes red. A new syntax feature
therefore lands twice, in the same round and with the same semantics: first
in `boot/src`, then (legally, once boot accepts it) in `self/rho.rho` and
the mirror's own sources. The differential corpus (e.g. `corpus/031_*` for
triple-quoted multiline strings) pins the shared semantics both ends must
agree on.

## The gate

`tools/gate.sh --wasm` is the acceptance run: boot selftest (AST + diag
goldens), build-mirror, corpus-diff (boot-built vs mirror-built artifacts
of every `corpus/*.rho` run under wasmtime, stdout + exit compared;
temporary `HEAP@`/`WE` debug lines are filtered), the self-chain (child,
grandchild, grandchild-rebuilds-corpus), and diag-parity
(`tests/diag/*.rho` messages byte-identical between boot and mirror). The
full run adds the native crossings (build-only image smokes and self-builds
per target) — those exercise the *mirror's* native backends, never boot's:
boot cannot emit a native image at all.

## The remaining surface

`boot/src`, one line per file — the whole of what is left:

| file | responsibility |
| --- | --- |
| `main.c` | CLI dispatch (`check`/`build`/`run`/`test`/`selftest`), `parse_target` (wasm32-wasi or clear error), the selftest goldens, the corpus test runner |
| `lex.c` | lexer: tokens, triple-quoted multiline strings |
| `parse.c` | parser: source text → AST |
| `check.c` | typechecker; registers the one prelude (core + wasi tail) into every compilation |
| `lower.c` | lowering: checked AST → SSA IR over virtual registers + frame slots |
| `ir.h` | the IR data model, the single-target `Target` enum, shared inline helpers |
| `emit_wasm.c` | the one backend: IR → binary wasm32-wasi module |
| `dump.c` | AST dumper behind the selftest's golden AST files (format shared with the mirror) |
| `util.c` | arena, `Str`/`SB`/`Vec`/`Map`, diagnostic rendering |
| `rho.h` | common definitions: arena, strings, AST/type declarations, symbol naming |
| `prelude_data.c` | generated (`tools/embed.py`): `core.rho` + `wasi.rho` embedded as one C string |

Dead-branch inventory from the slimming (what was removed, what stayed):

- Removed: `g_prelude_wasm` / `g_prelude_native` and the hosted/mac
  prelude selection — the target enum has one value, so target detection
  is dead; the lowerer's `wasm ? 4 : 8` pointer-width branches collapsed
  to `4` (wasm32); `boot/prelude/hosted.rho` and `mac.rho` are deleted,
  and boot embeds exactly one prelude (core + wasi). The mirror carries
  its own embedded prelude copies inside `self/rho.rho`, so deleting the
  boot-side tails does not starve it (note:
  `tools/embed-prelude.py` regenerates those copies from `boot/prelude/`
  and now has no hosted/mac source to read — it is a mirror-side tool,
  retired with this slimming).
- Kept: the `#if !defined(__wasm__)` shims in `main.c` (argv/`system`
  stub, tmp+rename atomic write, directory listing, run-temp naming) —
  boot itself still runs in two forms, the native `build/rho-boot`
  binary and `rho-boot.wasm` in the browser, and these are its own
  file/spawn shims for those two hosts, not target backends.

fmt is gone from boot (`fmt.c`, the `fmt` command, the selftest fmt
roundtrip, `make fmt-check`): canonicalization and roundtrip are the
self-built chain's job — `build/gate/m.wasm fmt <file>` (or the shipped
`rho.wasm`) is the formatter now, including any corpus canonicalization.
Corpus goldens stay frozen as behavior (stdout + exit), which never
needed fmt.

## Rebuild commands

```sh
make build/rho-boot                                        # the C seed (~10s)
./build/rho-boot build self/rho.rho --target wasm32-wasi \
  -o build/gate/m.wasm                                     # the mirror (60s cap)
make site                                                  # the site asset (the self-built chain)
make test                                                  # boot selftest (goldens + diag)
./build/rho-boot test corpus --target wasm32-wasi          # corpus vs goldens
tools/gate.sh --wasm                                       # the acceptance run
./build/rho-boot build x.rho --target arm64-mac            # clear error, exit 2
```

Every rho invocation in scripts runs under a wall-clock cap (`perl -e
'alarm shift; exec @ARGV' <seconds> <command>` — macOS has no GNU
`timeout`); the gate already wraps its own runs this way.
