# rho specification — memory, compilation, determinism, versions

The four documents together are the law: `syntax.md`,
`type-system.md`, `module-system.md`, and this one. The ratified design
(TODO.md) is the ceiling; nothing in these documents exceeds it.

## 1. Memory model

### 1.1 RC + weak

Reference counting with weak references. Every heap block carries a
**24-byte header** `{rc, wrc, drop}`:

- `rc` — strong count. `rc → 0` runs the generated destructor (`drop`),
  which releases owned fields, then the block dies.
- `wrc` — weak count. While `wrc > 0` the (dead) header stays alive so
  weak references can observe death (`weak.get()` returns `?*T`).
- Managed values: `*T` (structs/enums behind pointers), `string`,
  `[]T` slices, `dyn Trait`. Integers, floats, bools, and inline
  structs/enums are unmanaged.

No garbage collector is ever promised; no borrow checking is ever
promised — an evolution slot, not a commitment.

### 1.2 Zero UB is law

On the safe subset, there is **no undefined behavior**. Every
violation is a defined panic: out-of-bounds index, arithmetic panic
(`/0`, `%0`), `?` misuse caught at compile time, stack overflow.
The only unsafe window is the `intrinsics.` namespace (not in 0.1.0's
prelude surface; reserved).

### 1.3 Zeroed allocations

`new` (and `make`, where a buffer is grown) yields **zeroed** memory:
every byte reads as 0 before it is written — including reused blocks
from the allocator. A constructor that does not initialize a field
reads 0 (for a pointer field, that is the *non-null* violation case —
pointer fields must be initialized; see 1.5).

### 1.4 Counting = pure-local syntax insertion

Retains/releases are inserted mechanically:

- one **retain** per copy of a managed value into a longer-lived
  place (assignment, argument passing, field init, collection insert);
- one **release** per destruction of a managed value (scope exit,
  overwrite, container drop).

**Container ownership**: a container (struct/slice owning managed
fields) walks its elements only at the **rc==1 death check** — when a
block's rc drops to 1 and the last owning reference is being
destroyed, the destructor walks elements and releases each.
Redundant retain/release pairs may be eliminated by the optimizer
later; the baseline semantics are exact.

### 1.5 Pointers

`*T` is non-null. A `new T { … }` must initialize every pointer-typed
field (leaving one zeroed is a compile error for `*T`/string/slice
fields); integer/float/bool fields may be left to read 0. `?T` is
`Option[T]` (syntax.md §3); there is no `null`.

## 2. Errors

- **panic = a programmer bug, process-fatal.** No catch, no recover,
  no unwinding machinery — ever. A panic prints one line to stderr
  (`panic: <message>`) and exits with code **101**. Panic messages
  are deterministic (given the same input, byte-identical).
- **defer** runs on every scope exit path **except panic** (no
  unwinding to run it through).
- `Result[T, E]` / `Option[T]` + `?` are the only error channel.
  `?` propagates; `match` consumes (`type-system.md` §12).

## 3. Panic catalog (complete)

| panic            | when                                   |
| ---------------- | -------------------------------------- |
| `index out of bounds` | `s[i]` with `i >= len(s)`         |
| `division by zero`    | `/0`, `%0` on integers             |
| `stack overflow`      | unbounded recursion, cyclic `==`   |
| `unwrap on empty`     | prelude `Option.get()`-family on None (message names the operation) |

Stack overflow is a **defined panic** (exit 101, message
`panic: stack overflow`), detected via the wasm stack-exhaustion
path; the safe subset never traps with a raw wasm `unreachable`.

## 4. Operators and numbers

- Integers are two's complement and **wrap** on overflow. `MIN / -1 =
  MIN` (no trap), `/0` and `%0` panic. Shifts mask the shift count by
  the left operand's width (`1 << 33` on i32 shifts by 1).
- Floats are IEEE-754: `/0.0` = ±inf, `NaN != NaN`, no trapping.
- `&&`/`||` short-circuit; assignment is a statement (syntax.md §5).

## 5. Strings

- `string` is an immutable UTF-8 **byte slice**. `len`, indexing
  (`s[i]` yields `u8`), and slicing (`s[a:b]`) are byte-based; no
  built-in char abstraction (a utf-8 **package** may add code-point
  iteration).
- `+` concatenates strings and never coerces (no `string + int`).
- `printf`/`eprintf`/`format`: literal format string, `{}` placeholders
  counted at **compile time** (count mismatch = compile error), every
  value printed via its `to_str` (builtins have one; aggregates are
  not printable — passing a struct/enum/slice/Option/Result is a
  compile error).
- Triple-quoted strings are fully verbatim (syntax.md §2.5).
- Hot string building uses `[]u8` buffers (the `__fmt_build` pattern
  in the prelude); strings built per-character are an anti-pattern the
  optimizer may fix later.

## 6. Build parameters

- Root-file consts are ordinary consts (zero declaration restrictions)
  with **prelude status**: visible in every module; shadowing them is
  an error (module-system.md §9).
- `--set name=value` overrides any root const whose type has a text
  form: `bool`, all integer widths (`i8`…`u64`, `usize`), `f32`/`f64`,
  `string`. Values are range-checked against the const's type; a
  refusal (unknown name, malformed value, out of range) exits with
  code **2** and a diagnostic.
- **Comptime folding**: conditions over comptime-known values (root
  consts, literals, folded expressions) fold at compile time. Only the
  live branch is checked and emitted; dead branches are skipped whole
  (parsed, not checked). `use` statements in dead branches are pruned
  (reachability prunes modules).

## 7. Compilation model

- **0.1.0 has exactly one backend: wasm32-wasi.** No native targets.
- **boot** — the seed compiler written in C. Arena-allocated, one-shot
  (one process, one compile), implements every rule in these four
  documents, emits wasm. boot is the reference compiler; after 0.1.0
  it freezes with the language.
- **The self-hosted compiler** (`libs/compiler`) is written in rho,
  built by boot, and graded **behaviorally** against boot across the
  whole corpus: same program → same stdout + exit.
- **Trust root = pure-source rebuild.** Every gate run rebuilds the
  seed wasm from boot's C sources on the spot. The pinned `seed.wasm`
  in the repo is a **canary**: rebuilt output must match it
  byte-for-byte; inequality is a determinism alarm.
- Symbol policy: lib artifacts keep public names and minify internals;
  cli/app artifacts minify everything but `main`/`_start`; diagnostics
  never reference mangled names; `-g` keeps full names.

## 8. Determinism

- Same compiler + same version + same target + same input →
  **byte-identical output**: the wasm module, diagnostics, symbol
  order, folded constants — all of it. No timestamps, no hash-seed
  order, no environment-dependent behavior.
- Different backends / different compilers (boot vs self-hosted):
  bytes differ, **behavior must match** — stdout + exit code.
- Library containers have a deterministic, documented iteration order
  (std.collections).

## 9. Kernel boundary

The prelude holds **exactly the mechanism-required kernel**:

- `Option`/`Result` + `?` machinery,
- `to_str` family + variadic format sinks (printf/eprintf/format
  desugaring),
- panic/assert + hooks,
- allocator + rc glue,
- string primitives (`__streq`, concat),
- raw per-target syscall tails (fd_write/proc_exit/args/file io).

**The kernel grows only when a language mechanism grows.** Everything
else — collections, io (read_line lives in std.io, not the kernel),
json, utf-8 — is a package in `std/`.

## 10. Version policy

One version: **0.1.0**, tagged when the wasm self-hosting gate is
green. No intermediate numbers, no other tags, no evolution after the
freeze: this document set is the whole language. Post-0.1.0 growth is
libraries (utf-8, collections, io, net, native backends as
std-library components) and tooling quality — never language surface.

## 11. Conformance map — every rule → the test that holds it

Suites live in `tests/` (each a directory of `.rho` programs with
expected stdout+exit, run through the full compiler); corpus programs
live in `corpus/`. Rule ids: §n = section of this document, S/T/M =
syntax/type/module document.

| rule | test |
| ---- | ---- |
| S2.2 int literals, defaults | `tests/lang/literals_int.rho`, corpus 001/002 |
| S2.5 verbatim triple-quoted | `tests/multiline/*`, corpus 031 (adapted: escapes verbatim) |
| S2.5 escape set exact | `tests/lang/escapes.rho` |
| S4.1 overloads parse | `tests/lang/overload_parse.rho` |
| S4.4 use one form / pub use four | `tests/modsys/use_forms.rho`, `tests/modsys/facade_reexport.rho` |
| S5 inner shadowing legal | `tests/lang/shadow_inner.rho` |
| S5 same-scope rebind error | `tests/diag/rebind.rho` (must fail) |
| S5 labels break/continue | `tests/lang/labels.rho` |
| S5 compound assign bitwise | `tests/lang/compound_bitwise.rho` |
| S6.1 precedence table | `tests/lang/precedence.rho` |
| T1 consumer-typed literals | `tests/lang/literal_consumers.rho` |
| T2 const inference/pin | `tests/lang/const_infer.rho` |
| T3 `as` unified truncation (const == var) | `tests/lang/as_const_var.rho`, corpus 048–055 |
| T5 overload exact-match-unique | `tests/lang/overload_resolve.rho`, `tests/diag/overload_ambig.rho` |
| T6 method visibility import-scoped | `tests/modsys/method_visibility.rho` |
| T8 bounds per instantiation | `tests/lang/generic_bounds.rho`, corpus 081 |
| T9 `?T` sugar, non-null | `tests/lang/opt_sugar.rho`, `tests/diag/null_use.rho` |
| T10 `==` law | `tests/eq/*`, corpus 056–058 |
| T12 `?` on both | corpus 077 (adapted), `tests/lang/qmark_mix.rho` (must fail) |
| §1.1 rc/weak header | corpus 090/091/099 (adapted) |
| §1.3 zeroed allocations | corpus 102 (adapted), `tests/lang/make_zero.rho` |
| §1.4 counting insertion | corpus 090/092/093 (adapted) |
| §2/§3 panic catalog | corpus 043/044/094, `tests/lang/panic_stack.rho` |
| §4 wrap/MIN/shift-mask/IEEE | corpus 040–047, 013 |
| §5 strings byte semantics | corpus 060–069 (adapted), `tests/strops/*` |
| §6 --set widened face | `tests/params/*` |
| §6 comptime folding + pruning | `tests/params/fold_dead_branch.rho` |
| §7 wasm32-wasi only | gate legs (gate.sh) |
| §8 byte-identical determinism | gate: seed rebuild vs canary |
| §8 cross-compiler behavioral | gate: corpus differential |
| §9 kernel exact set | audit test `tests/kernel_audit.rho` (negative: no extra symbols) |
| §10 one version | release check in gate |

Diagnostic ("must fail") programs are `.rho` files whose expected
result is a compile error carrying a named substring; the harness
checks both the failure and the message. Every test id above exists by
the end of Phase 3 — a rule whose test does not exist is a rule not
implemented.
