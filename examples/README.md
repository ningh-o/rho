# examples/ — the teaching programs

Promoted from `corpus/` at the freeze (T6.4): whole programs that read
like the language's greatest hits — structs, enums and match, defer,
floats, generics, `?`, closures, variadics, traits, dyn dispatch,
composition, zeroed allocations, the `Option` chain walk, and
`std.collections`. Every one is user-facing documentation; browse them
to see the language, run any of them:

```sh
./build/rho run examples/006_enums_match.rho
```

They cannot rot: `make test` runs `tools/verify-examples.mjs`, which
compiles and executes each program through the real compiler and
matches its `.out` golden byte-for-byte together with the `// exit:`
header. Editing an example into non-compiling or dishonest shape
fails the same battery the compiler must pass.

The rest of the corpus's behavioral memory lives one floor down:
`tests/suites/programs/` — the whole-program pins the differential
grades boot against the self-hosted compiler on.
