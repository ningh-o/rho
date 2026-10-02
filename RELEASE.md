# rho 0.1.0 — release notes

The one and only version. rho is a small, hand-forged systems
language: statically typed, reference-counted (no garbage collector,
no use-after-free), with **zero undefined behavior** — integer
overflow wraps, every slice index is bounds-checked, and every
violation is a *defined* panic (`panic: <message>` on stderr, exit
101) from a catalog you can count on one hand. There is no null:
absence is `Option[T]` (spelled `?T`) and recoverable errors are
`Result[T, E]` with the `?` operator.

## What is in this zip

| file         | what it is                                                        |
| ------------ | ----------------------------------------------------------------- |
| `rho.wasm`   | the rho compiler — the self-hosted one, as a wasm32-wasi module    |
| `README.md`  | this file                                                          |
| `LICENSE`    | the license                                                        |

`rho.wasm` is the real compiler: the full rho source of
`libs/compiler` is compiled *by rho itself* (bootstrapped from
`boot`, the C seed in the repository), and it is graded against boot
across the whole corpus on every change — same program in, same
stdout and exit code out. The reserved standard library (`std.io`,
`std.collections`, `std.json`, `std.utf8`) is baked in.

## Using it

The binary reads `/main.rho` from the directory you hand it (plus an
optional `/mode` marker), compiles it, and writes the module as
canonical WebAssembly text to stdout. Any WASI runtime works; the
examples use [wasmtime](https://wasmtime.dev), and `wat2wasm` from
[wabt](https://github.com/WebAssembly/wabt) assembles the text:

```sh
mkdir hello && cd hello
cat > main.rho <<'EOF'
fn main() -> i32 {
  printf("hello, world\n");
  return 0;
}
EOF
wasmtime run --dir . rho.wasm > hello.wat     # mode marker absent: build
wat2wasm hello.wat -o hello.wasm
wasmtime hello.wasm                           # hello, world
```

The `/mode` marker selects the other verbs: write `check` into it for
the type-checker's diagnostics (exit 0 clean, exit 1 with
`file:line:col: error: …` on stderr), or `fmt` to print the canonical
formatting (its own output re-parses byte-for-byte identically). A
program that panics prints `panic: <message>` and exits 101.

The whole language — the memory model, the counting law, the panic
catalog, every rule with the test that holds it — is the
specification, served at <https://ningh-o.github.io/rho/spec.html>
alongside a browser playground that runs this exact compiler and a
seventeen-chapter tutorial.

## The local toolchain

The repository builds a native `rho` binary with one C compiler and
`make` — that binary (`boot`, the seed) implements every verb
(`build`, `run`, `test`, `fmt`, `check`) end to end, executing
programs in-process with no external runtime:

```sh
git clone https://github.com/ningh-o/rho && cd rho
make
echo 'fn main() -> i32 { printf("hi\n"); return 0; }' > hello.rho
./build/rho run hello.rho
```

## Determinism, and how this release was held

Same compiler, same version, same target, same input →
**byte-identical output**. That is a tested invariant, not a promise:
every gate run rebuilds the compiler from source and byte-compares it
against a pinned canary, runs a 122-case behavioral corpus
differential between boot and the self-hosted compiler, replays 150
seeded fuzz programs through both, sweeps the corpus under
ASAN+UBSAN, and closes the self-hosting chain (mirror → child →
grandchild, byte-identical at depth). The full conformance map —
every rule of the specification next to the test that holds it — is
the last section of the overview spec.

## The freeze

0.1.0 is the whole language and the only version. From here the
language grows no more; growth is libraries and tooling quality. The
backend is exactly one: wasm32-wasi. There are no intermediate
numbers and no other tags.
