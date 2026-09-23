# json — a JSON codec for rho

A JSON encoder/decoder written in rho, and the first real package in the
rho package ecosystem: a manifest (`rho.toml`) plus one entry module
(`json.rho`), consumable by any package through the rho-pkg
path-dependency workflow ([docs/package-manager.md](../../docs/package-manager.md)).

## Consuming it

Vendor the package (a copy or a git clone — rho-pkg consumes path
dependencies in place), wire it into your manifest, and pass the frozen
gate:

```sh
mkdir -p vendor
cp -r <this directory> vendor/json     # or: git clone ... vendor/json
printf 'add json --path vendor/json\n' | wasmtime run --dir . rho-pkg.wasm
printf 'install --frozen\n'            | wasmtime run --dir . rho-pkg.wasm
```

Then it is an ordinary module import — no flags, no config:

```rho
use vendor/json/json;

fn main() -> i32 {
  let out = json.new_out();
  let r = json.parse("{\"hello\": [1, 2.5, \"world\"]}", out);
  if r.is_err() {
    eprintf("parse failed: {}\n", json.err_msg(r));
    return 1;
  }
  // errors look like: `3:2 (offset 9): unexpected character (a value
  // cannot start here)` — line, byte column, byte offset
  let e = json.encode(out.root);
  printf("{}\n", json.err_text(e));
  return 0;
}
```

A worked consumer — manifest edit, lockfile, frozen gate, and a run —
lives in `tests/run.sh` (stage 3) and is exercised on every test pass.

## The model

One in-memory tree of heap nodes:

- `*Json` — a value handle; the variant rides in `v: Jv`
  (`Null`, `Bool`, `Num(f64)`, `Str`, `Arr`, `Obj`).
- `*Member` — one object pair (`key: string`, `val: *Json`).
- `*Parsed` — caller-owned out struct parse reports through (the boot
  compiler cannot express `Result[*T, ...]`; see "Constraints" below).

Builders: `mk_null`, `mk_bool`, `mk_num`, `mk_str`, `mk_arr`,
`mk_obj`, `mk_member`. Predicates: `is_null` … `is_obj`. Extractors:
`as_bool`, `as_num`, `as_str` (zero value on the wrong kind — pair them
with the predicates), `arr_at` (null when out of bounds), `obj_get`
(null when the key is missing — a present JSON `null` is a distinct,
non-null pointer holding `Jv.Null`). Mutation: `arr_push` appends in
place; `obj_set` appends or replaces the first member with the key.
`deep_eq` is structural equality: order-insensitive objects
(first match on duplicates), order-sensitive arrays, numbers by f64
`==` (nan equals nothing, `-0.0` equals `0.0`). `size`, `items`,
`members` expose containers.

## Semantics

**Strings are UTF-8 byte strings** — exactly rho's `string`.
- Decode validates UTF-8 strictly inside strings: bad continuation
  bytes, overlong forms, UTF-16 surrogates, and anything past U+10FFFF
  are errors; raw control bytes (< 0x20) are errors.
- Decode accepts the full escape set: `\" \\ \/ \b \f \n \r \t` and
  `\uXXXX` in any hex case, with surrogate pairs combining to astral
  code points (`\udbff\udfff` decodes to U+10FFFF). Lone halves are
  errors. NUL (`\u0000`) is a legal string byte.
- Encode emits the minimal standard escapes — `\" \\` and the C0
  controls (named for `\b \f \n \r \t`, `\u00xx` for the rest) — and
  passes every other byte through, including raw UTF-8 and `/`.
  It never re-escapes what decode un-escaped beyond that, so
  encode → parse → encode is a fixpoint.

**Numbers are f64 end to end.**
- Decode is correctly rounded (round half to even) over the whole
  double range — exact bigint arithmetic, no libc strtod on any target:
  subnormals down to the deep-underflow window, the `2.2250738585072014e-308`
  boundary, overflow to the infinities, `-0` keeping its sign, and
  800 significant digits of headroom so every exact rounding tie a
  double can have is decided exactly.
- Encode prints the prelude's canonical form (17 significant digits,
  integral values keeping their `.0`) and round-trips bit-exactly
  through decode.
- `nan` and the infinities parse from nothing and encode to nothing:
  encode returns `Err` naming them.

**Parsing is strict.** The JSON grammar and nothing more: no leading
zeros, no trailing garbage after the top-level value, no `+1`, `.5`,
`1.`, `1e`, `Infinity`, `NaN`, hex numbers, comments, or single quotes.
Duplicate keys are kept in parse order; the first wins for lookup.

**Errors carry positions.** Every parse failure is a single line of the
form `line:col (offset N): reason` — 1-based line and byte column,
byte offset from the document start — e.g.
`3:2 (offset 9): unexpected character (a value cannot start here)`.

**Depth is bounded.** Nesting deeper than `MAX_DEPTH` (512) containers
is a positioned error, not a stack overflow (the recursive-descent
parser would otherwise corrupt wasm linear memory past the stack
budget — measured against the reference boot compiler). The limit is
parse-only: a hand-built deeper tree still encodes.

## API sketch

```rho
// decode: Result[string, string]; the tree lands in out.root
pub fn parse(src: string, out: *Parsed) -> Result[string, string]

// encode: Result[string, string] — only non-finite numbers fail
pub fn encode(v: *Json) -> Result[string, string]
pub fn encode_pretty(v: *Json) -> Result[string, string]  // 2-space indent

// status accessors for the two Results above
pub fn err_text(r: Result[string, string]) -> string
pub fn err_msg(r: Result[string, string]) -> string
```

## Testing

```sh
sh rho/libs/json/tests/run.sh
```

Three stages, self-contained (no `make`, no compiler corpus; artifacts
in a temp dir only):

1. the package typechecks and is fmt-canonical under the reference
   boot compiler;
2. nine case programs build and run under wasmtime, each diffed
   byte-for-byte against a committed `.out` golden — encode scalars and
   the escape set, containers and buffer growth, the valid grammar, the
   full escape set (decoded bytes dumped in decimal), a 63-input error
   catalog with pinned positions, 64 number literals pinned to the
   exact f64 bit patterns of an IEEE strtod oracle, bit-exact round
   trips, the model API, and the depth boundary (512 in, 513 out);
3. a fresh consumer package goes through the real rho-pkg workflow —
   `add --path`, `install --frozen`, then `rho run` — proving the
   package is consumable as a path dependency.

## Constraints this shape works around

Probed against `./build/rho-boot` (rho 0.4.0) and re-probed against the
self-hosted mirror on 2026-09-23 (the era of the pinned seed) — all three
constraints hold on both ends:

- `new` builds structs only, so the enum payload rides in
  `Json.v` and every value is a `*Json` handle.
- Generic type arguments must be primitives (`Result[*T, ...]` is not
  expressible), so parse reports through the caller-owned `Parsed` out
  struct — the same idiom `tools/pkg/rho-pkg.rho` uses.
- A `const` string has no address and cannot be dynamically indexed,
  so lookup tables are local literals inside their functions.
