# wasm32-wasi backend — locked design (0.0.4)

Implementation is a straight transcription of this document.

## Module shape
- Sections in order: type(1), import(2), function(3), memory(5), global(6),
  export(7), code(10), data(11).
- Imports (module "wasi_snapshot_preview1"): fd_write (i32 x4) -> i32,
  proc_exit (i32) -> (). funcidx 0 and 1.
- Every checked rho fn with a body becomes a wasm function, in module
  registration order (same order as native emission — deterministic).
- Exports: "memory" (memidx 0) and "_start" (calls the root module's main,
  then proc_exit(result); void main exits 0).
- Memory: min pages = max(2, data_end/65536 + 2). Data segment at 1024.

## Linear memory layout
1024 | statics (round 8) | literal records (24B) + literal bytes
| heap_base = align16(g_wend); __alloc bump-allocates there (wasi prelude
static HEAP initialized via a data segment at layout time).

## Function shape
- Params: scalars -> wasm params of their valtype; aggregates -> i32
  (caller passes a pointer to its own copy).
- Prologue: copy aggregate params from the pointer into a private frame
  slot (two-slot scheme, mirrors native); reserve the frame:
  global.get $sp; local.set $fb; global.get $sp; i32.const K; i32.add;
  global.set $sp. K = total frame bytes (known after the body is emitted:
  assemble the body into a scratch SB, then prepend locals + prologue).
- Scalar lets -> wasm locals (typed). Aggregate lets -> frame slots
  (address = $fb + off). Bindings resolve innermost scope first.
- Expressions leave values on the operand stack: scalars push their value,
  aggregates push an i32 address of a materialized value. Aggregate
  rvalues materialize into a fresh frame scratch region (we_frame bump).
- String literals: 24-byte slice record {litaddr, litaddr, len} laid out
  in data at layout time, keyed by Expr*; the expression is i32.const.
- Calls: direct `call funcidx`; args evaluated left to right (scalars push
  value, aggregates push address); aggregate results arrive as an i32
  address on the stack.
- Control flow maps 1:1 to the source structure:
  - if statement: condition; if (void) [then] else [else] end
  - if expression: if (result T) / match: nested if/else chain on the tag
  - while: block $exit { loop cond; br_if $exit; body; br $loop }
  - loop: block $exit { loop body; br $loop }
  - break/continue: br to the recorded labeled-block depth
  - defer: scope-stacked, run at scope exit / return / break / continue
    (same rules as native lowering)
- Depth bookkeeping: one counter of open labeled constructs; br depth =
  depth - 1 - block_index (block_index assigned at open time).
- Divide/mod: evaluate divisor, guard calling __panic_div (a prelude fn),
  then i32/i64 div_s/u, rem_s/u. Indexing: bounds check via __panic_oob.
  Null deref: __panic_null. All three live in the wasi prelude.

## Prelude (boot/prelude/wasi.rho — landed)
fd_write/proc_exit imports; Iov struct for iovecs; HEAP static bump
allocator (__alloc/__free); print/eprint build an iovec via `new Iov` and
call fd_write; panic/__panic_* exit via proc_exit(101).

## Checker dependency (landed)
`as` allows pointer <-> usize/i64 (the sanctioned escape hatch used by the
prelude iovec plumbing).

## Testing
`rho test --target wasm32-wasi` builds .wasm and runs it under wasmtime
(exit codes propagate through WASI). Stdout captured to the .out files
like native. wasmtime is already on PATH (homebrew).
