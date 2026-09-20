# rho (ρ)

A small, hand-forged systems language. One binary toolchain, direct machine
code emission, reference-counted memory, zero undefined behavior. Targets:

- `wasm32-wasi` — a runnable module, end to end through any WASI runtime;
- `arm64-mac` — a static, ad-hoc-signed Mach-O built entirely in-tree
  (own assembler, own linker, own SHA-256 code signature);
- `amd64-linux`, `arm64-linux` — static ELF64 images, no interpreter, no
  libc, raw syscalls (built in-tree the same way; libc stays allowed for
  targets that want it);
- `amd64-mac` — the SysV backend's Rosetta test vehicle, emits `.s`.

Nothing in the toolchain shells out to an assembler, linker, or codesign:
`build` for an image target writes a runnable file in one process.

```
rho build main.rho      # wasm32-wasi: a.wasm; image targets: a runnable
                       # static binary; amd64-mac: assembly (.s)
rho run   main.rho      # build & execute (wasm32-wasi via a WASI runtime;
                       # image targets natively on a matching host)
rho test                # compile + run the corpus
rho fmt   main.rho      # canonical formatting (in place with -w)
rho check main.rho      # type-check only
```

## Status

| Version | Milestone |
|---------|-----------|
| 0.0.x   | Boot compiler, written in C (`boot/`), emits native + wasm directly |
| 0.0.6   | Tutorial site + browser playground |
| 0.1.0   | Self-hosting: compiler rewritten in rho (`self/`), zero C in the shipped toolchain |
| 0.2.x   | esp32c3 target, soft-float prelude (superseded — removed in 0.3.1) |
| 0.3.0   | Printing by type: `to_str` on every primitive, generic `print`/`println`/`eprint`, one shared prelude core, dead-fn elimination, full-width f32/u8 codegen fixes on all backends. |
| 0.3.1   | The esp32c3 backend is removed and the toolchain stops shelling out to the system `cc`: native targets emit assembly and stop; `run`/`test` execute wasm32-wasi through a WASI runtime. |
| 0.3.2   | Native images, end to end: in-tree assemblers (arm64 + amd64), a static ELF writer for the linux targets, a static Mach-O writer with the ad-hoc code signature for arm64-mac, freestanding runtime blobs. The bootstrap gate reopens while the mirror catches up (`docs/todo.md`) — `boot` remains the reference toolchain. |
| 0.3.3   | Formatted printing: `printf`/`eprintf` with `{}` placeholders over the `to_str` protocol, variadic functions (`rest: T...`, spread `xs...`) for every program, and the `print`/`println`/`eprint` trio removed. The spec splits into [spec/syntax.md](spec/syntax.md), [spec/type-system.md](spec/type-system.md), and [spec/module-system.md](spec/module-system.md). |
| 0.3.4   | `format(fmt, ...) -> string` — the printf desugar pointed at a string sink; `to_str` bodies become one format line. |

The C compiler under `boot/` is the permanent bootstrap seed: it exists to
compile the first self-hosted compiler and nothing else. Everything after
0.1.0 is built by rho itself (stage2 == stage3, byte for byte).

See [spec/spec.md](spec/spec.md) for the specification overview — the surface grammar in [spec/syntax.md](spec/syntax.md), the type rules in [spec/type-system.md](spec/type-system.md), program composition in [spec/module-system.md](spec/module-system.md).
