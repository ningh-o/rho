# rho

A small, hand-forged systems language. One binary toolchain,
reference-counted memory, zero undefined behavior, deterministic
compilation. The 0.1.0 target is the wasm self-hosting bootstrap:
a C seed compiler (`boot`) → the compiler rewritten in rho → it
compiles itself to wasm.

**Status: pre-0.1.0 rebuild in progress.** The plan — and the ratified
language design that binds every implementation decision — lives in
[TODO.md](TODO.md). The previous implementation is preserved on the
`archive/pre-0.1.0` branch (reference only).

## Layout

| path      | what                                        |
| --------- | ------------------------------------------- |
| `boot/`   | the C seed compiler (reference compiler)    |
| `spec/`   | the language law: syntax, types, modules    |
| `corpus/` | behavioral golden corpus (stdout + exit)    |
| `libs/`   | the self-hosted compiler + std packages     |
| `tests/`  | language suites (lang/modsys/opt/eq/…)      |

## License

MIT — see [LICENSE](LICENSE).
