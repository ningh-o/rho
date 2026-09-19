# rho (ρ)

A small, hand-forged systems language. One binary toolchain, direct machine
code emission, reference-counted memory, zero undefined behavior. Targets:
`amd64-linux`, `arm64-mac`, `wasm32-wasi`; ESP32 (RISC-V / Xtensa) later.

```
rho build main.rho      # compile & link
rho run   main.rho      # compile & execute
rho test                # run all test_* functions
rho fmt   main.rho      # canonical formatting (in place with -w)
rho check main.rho      # type-check only
```

## Status

| Version | Milestone |
|---------|-----------|
| 0.0.x   | Boot compiler, written in C (`boot/`), emits native + wasm directly |
| 0.0.6   | Tutorial site + browser playground |
| 0.1.0   | Self-hosting: compiler rewritten in rho (`self/`), zero C in the shipped toolchain |
| 0.2.x   | esp32c3 target, soft-float prelude |
| 0.3.0   | Printing by type: `to_str` on every primitive, generic `print`/`println`/`eprint`, one shared prelude core, dead-fn elimination, full-width f32/u8 codegen fixes on all backends. The self-hosted mirror of this work is the immediate next step (`docs/todo.md`) — until it lands, `boot` is the reference toolchain and the bootstrap gate is open. |

The C compiler under `boot/` is the permanent bootstrap seed: it exists to
compile the first self-hosted compiler and nothing else. Everything after
0.1.0 is built by rho itself (stage2 == stage3, byte for byte).

See [spec/spec.md](spec/spec.md) for the language specification.
