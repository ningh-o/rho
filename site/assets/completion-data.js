// Static completion tables for the playground: language keywords, primitive
// types, compiler builtins, and the user-facing prelude surface of the
// wasm32-wasi target the site compiles for.
//
// FAST COMPLETION, regex-grade: this data is hand-maintained and typed from
// the authoritative sources (spec/syntax.md §1/§2/§3, boot/prelude.rho,
// std/io.rho, std/collections/). The type-aware completion that will replace
// these tables is specified in docs/language-service.md — the controller in
// the editor core keeps an adapter slot for it.

// One completion entry: { label, kind, detail, doc }.
//   kind   — short tag rendered in the popup (kw / ty / fn / enum / trait /
//            struct / pkg)
//   detail — one-line signature, shown next to the label
//   doc    — one-sentence English documentation, shown as the popup hint

// Reserved words (spec/syntax.md §2.1). `null` is reserved AND removed —
// pointers are non-null, absence is ?T — so it is deliberately not offered.
// Ordered for completion, not alphabetically: related words sit together.
export const KEYWORDS = [
  ["fn", "fn name(params) -> T { … }", "declares a function; fn T.name(self, …) is a method"],
  ["let", "let x: T = …;  let mut x: T = …;", "immutable by default; mut allows assignment"],
  ["mut", "let mut x: T = …;", "marks a binding as assignable"],
  ["return", "return expr;", "returns from the enclosing function"],
  ["if", "if cond { … } else { … }  (an expression)", "conditional; both arms must produce the same type"],
  ["else", "if cond { … } else { … }", "alternative arm of if"],
  ["while", "while cond { … }", "pretested loop"],
  ["loop", "loop { … }", "infinite loop; exit with break"],
  ["break", "break;  break label;", "leaves the innermost (or labeled) loop"],
  ["continue", "continue;", "jumps to the next loop iteration"],
  ["defer", "defer expr;", "runs expr on every exit of the enclosing scope, LIFO"],
  ["match", "match v { P => e, … }  (an expression)", "exhaustive pattern match; Variant(p) or full Path.Variant(p)"],
  ["test", 'test "name" { … }', "declares one test — judged by assert vs clean return (rho test)"],
  ["struct", "struct Name { field: T, … }", "declares a record type"],
  ["enum", "enum Name { Variant(T), … }", "declares a tagged-union type"],
  ["trait", "trait Name { fn m(self) -> T, … }", "declares a protocol types can satisfy"],
  ["impl", "impl Name for T { … }", "satisfies a trait for a type"],
  ["new", "new T { field: e, … }", "allocates a zeroed heap struct (reference-counted)"],
  ["true", "let b: bool = true;", "the true literal"],
  ["false", "let b: bool = false;", "the false literal"],
  ["as", "x as T", "the one conversion — numeric, enum-to-tag, pointer casts"],
  ["use", "use std.io;", "imports a module or package by dot path"],
  ["pub", "pub fn … / pub use …", "exports an item beyond its module"],
  ["static", "static mut NAME: T = …;", "the only global state — module lifetime, mutable"],
  ["const", "const NAME: T = …;", "compile-time constant; root-file consts are build parameters"],
  ["self", "fn T.m(self: *T, …)", "method receiver; mut self grants write access (§18 view law)"],
  ["extern", "extern name: fn(…) -> T;", "declares a foreign function"],
  ["Self", "trait Eq { fn eq(self, other: Self) -> bool }", "inside a trait/impl: the type satisfying or being implemented"],
  ["dyn", "let s: []dyn Shape = make(…)", "trait object — dynamic dispatch through a vtable"],
  ["for", "impl Eq for T { … }", "part of the impl form"],
].map(([label, detail, doc]) => ({ label, kind: "kw", detail, doc }));

// Primitive type names (spec/syntax.md §3; mirrors the highlighter's TYPES set).
export const PRIMITIVE_TYPES = [
  ["bool", "true / false"],
  ["i8", "-128 … 127"],
  ["i16", "-32768 … 32767"],
  ["i32", "32-bit signed integer (the literal default)"],
  ["i64", "64-bit signed integer"],
  ["u8", "0 … 255"],
  ["u16", "0 … 65535"],
  ["u32", "32-bit unsigned integer"],
  ["u64", "64-bit unsigned integer"],
  ["usize", "unsigned, address-width; slice indices and len results"],
  ["f32", "32-bit IEEE float"],
  ["f64", "64-bit IEEE float (the float-literal default)"],
  ["string", "immutable UTF-8 byte slice; + concatenates, never coerces"],
].map(([label, doc]) => ({
  label,
  kind: "ty",
  detail: label,
  doc: "primitive type — " + doc,
}));

// Compiler builtins: recognized by name at call sites, reserved, no import
// needed (spec/module-system.md).
export const BUILTINS = [
  {
    label: "printf",
    kind: "fn",
    detail: "printf(fmt, …)",
    doc: "formatted stdout; each {} in the string literal takes the next value — the count is checked at compile time",
  },
  {
    label: "eprintf",
    kind: "fn",
    detail: "eprintf(fmt, …)",
    doc: "printf to stderr",
  },
  {
    label: "format",
    kind: "fn",
    detail: "format(fmt, …) -> string",
    doc: "like printf but produces the string instead of printing it",
  },
  {
    label: "len",
    kind: "fn",
    detail: "len(x) -> usize",
    doc: "slice / string length",
  },
  {
    label: "make",
    kind: "fn",
    detail: "make([]T, n) -> []T",
    doc: "new zeroed slice of n elements",
  },
  {
    label: "panic",
    kind: "fn",
    detail: "panic(msg)",
    doc: "prints panic: msg to stderr and exits with code 101 — no catch, ever",
  },
];

// Prelude surface (boot/prelude.rho — the kernel every program embeds).
// ?T is Option[T] sugar; string concatenation is the `+` operator; nothing
// coerces to string. std packages live below.
export const PRELUDE = [
  {
    label: "assert",
    kind: "fn",
    detail: "assert(cond: bool)",
    doc: "panics with `assertion failed` when cond is false",
  },
  {
    label: "Option",
    kind: "enum",
    detail: "enum Option[T] { Some(T), None }",
    doc: "optional value; ?T is the sugar; methods is_some() / is_none()",
  },
  {
    label: "Result",
    kind: "enum",
    detail: "enum Result[T, E] { Ok(T), Err(E) }",
    doc: "recoverable error; methods is_ok() / is_err(); propagate with ?",
  },
  {
    label: "Eq",
    kind: "trait",
    detail: "trait Eq { fn eq(self, other: Self) -> bool }",
    doc: "== and != resolve through it — slotwise default, an impl overrides",
  },
  {
    label: "Ord",
    kind: "trait",
    detail: "trait Ord { fn lt(self, other: Self) -> bool }",
    doc: "the ordered four resolve through it — explicit impl only",
  },
  {
    label: "Hash",
    kind: "trait",
    detail: "trait Hash { fn hash(self, h: *Hasher) }",
    doc: "byte-exact default, an impl overrides — std.collections maps key through it",
  },
  {
    label: "weak",
    kind: "fn",
    detail: "weak.from(p: *T) -> weak[T]",
    doc: "non-owning handle; w.get() -> ?T reads None once the object dies",
  },
];

// std/ — imported by dot path; the tree ships baked into the site compiler.
export const STDLIB = [
  {
    label: "std.io",
    kind: "pkg",
    detail: "use std.io;",
    doc: "read_line() -> Result[?string, IoError] (newline stripped; Ok(None) at end of input), read_file, write_file",
  },
  {
    label: "std.collections",
    kind: "pkg",
    detail: "use std.collections;",
    doc: "Vec, StringMap, IntMap — deterministic iteration order, documented in the spec",
  },
  {
    label: "std.json",
    kind: "pkg",
    detail: "use std.json;",
    doc: "a JSON parser and serializers over the exact-decimal number core",
  },
  {
    label: "std.utf8",
    kind: "pkg",
    detail: "use std.utf8;",
    doc: "code-point iteration over UTF-8 strings",
  },
];

// The static source as one flat list. Order matters only within one
// match tier: the rank field breaks ties, and every static rank sits above
// the rank 0 that scanSymbols emits for document symbols — locals from the
// buffer always outrank these (see completion-core.js filterItems).
export const STATIC_ITEMS = [
  ...BUILTINS.map((it) => ({ ...it, rank: 1 })),
  ...PRELUDE.map((it) => ({ ...it, rank: 2 })),
  ...STDLIB.map((it) => ({ ...it, rank: 2 })),
  ...PRIMITIVE_TYPES.map((it) => ({ ...it, rank: 3 })),
  ...KEYWORDS.map((it) => ({ ...it, rank: 4 })),
];
