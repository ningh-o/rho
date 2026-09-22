// Static completion tables for the playground: language keywords, primitive
// types, compiler builtins, and the user-facing prelude surface of the
// wasm32-wasi target the site compiles for.
//
// FAST COMPLETION, regex-grade: this data is hand-maintained and typed from
// the authoritative sources (spec/syntax.md, spec/module-system.md,
// boot/prelude/core.rho). The type-aware completion that will replace these
// tables is specified in docs/language-service.md — the controller in
// completion.js keeps an adapter slot (registerProvider) for it.

// One completion entry: { label, kind, detail, doc }.
//   kind   — short tag rendered in the popup (kw / ty / fn / enum / trait /
//            struct / let / const)
//   detail — one-line signature, shown next to the label
//   doc    — one-sentence English documentation, shown as the popup hint

// Reserved words (spec/syntax.md "Keywords", incl. the trait/impl/for/dyn
// reserve note). Ordered for completion, not alphabetically: related words
// sit together so a bare keyword trigger reads like a menu.
export const KEYWORDS = [
  ["fn", "fn name(params) -> T { … }", "declares a function"],
  ["let", "let x: T = …;  let mut x: T = …;", "immutable by default; mut allows assignment"],
  ["mut", "let mut x: T = …;", "marks a binding as assignable"],
  ["return", "return expr;", "returns from the enclosing function"],
  ["if", "if cond { … } else { … }  (an expression)", "conditional; both arms must produce the same type"],
  ["else", "if cond { … } else { … }", "alternative arm of if"],
  ["while", "while cond { … }", "pretested loop"],
  ["loop", "loop { … }", "infinite loop; exit with break"],
  ["break", "break;", "leaves the innermost loop"],
  ["continue", "continue;", "jumps to the next loop iteration"],
  ["defer", "defer expr;", "runs expr on every exit of the enclosing scope"],
  ["match", "match v { P => e, … }  (an expression)", "exhaustive pattern match"],
  ["struct", "struct Name { field: T, … }", "declares a record type"],
  ["enum", "enum Name { Variant(T), … }", "declares a tagged-union type"],
  ["trait", "trait Name { fn m(self) -> T, … }", "declares a protocol types can satisfy"],
  ["impl", "impl Name for T { … }", "satisfies a trait for a type (eager check)"],
  ["new", "new T { … }", "allocates a heap struct (reference-counted)"],
  ["null", "let p: *T = null;", "the null pointer literal"],
  ["true", "let b: bool = true;", "the true literal"],
  ["false", "let b: bool = false;", "the false literal"],
  ["as", "x as T", "explicit cast between numeric/pointer types"],
  ["use", "use module;", "imports another module into the graph"],
  ["pub", "pub fn … / pub struct …", "exports an item beyond its module"],
  ["static", "static NAME: T = …;", "module-level storage"],
  ["const", "const NAME: T = …;", "compile-time constant"],
  ["self", "fn T.m(self: *T, …)", "method receiver; first named self parameter"],
  ["weak", "weak p: *T", "non-owning pointer that cannot keep an object alive"],
  ["extern", "extern fn name(…) -> T;", "declares a foreign (libc-style) function"],
  ["for", "reserved", "reserved word (trait/impl/for/dyn are reserved by the spec)"],
  ["dyn", "reserved", "reserved word (trait/impl/for/dyn are reserved by the spec)"],
].map(([label, detail, doc]) => ({ label, kind: "kw", detail, doc }));

// Primitive type names (spec §2; mirrors the highlighter's TYPES set).
export const PRIMITIVE_TYPES = [
  ["bool", "true / false"],
  ["i8", "-128 … 127"],
  ["i16", "-32768 … 32767"],
  ["i32", "32-bit signed integer"],
  ["i64", "64-bit signed integer"],
  ["isize", "signed, pointer-sized"],
  ["u8", "0 … 255"],
  ["u16", "0 … 65535"],
  ["u32", "32-bit unsigned integer"],
  ["u64", "64-bit unsigned integer"],
  ["usize", "unsigned, pointer-sized; slice indices"],
  ["f32", "32-bit IEEE float"],
  ["f64", "64-bit IEEE float"],
  ["string", "{ ptr: *u8, len: usize } slice of UTF-8 bytes"],
].map(([label, doc]) => ({
  label,
  kind: "ty",
  detail: label,
  doc: "primitive type — " + doc,
}));

// Compiler builtins (spec/module-system.md "Compiler builtins"): recognized
// by name at call sites, reserved, no import needed.
export const BUILTINS = [
  {
    label: "printf",
    kind: "fn",
    detail: "printf(fmt, …)",
    doc: 'formatted stdout; each {} in a string literal takes the next value',
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
    doc: "array / slice / string length",
  },
  {
    label: "make",
    kind: "fn",
    detail: "make([]T, n) -> []T",
    doc: "new zeroed array of n elements",
  },
  {
    label: "panic",
    kind: "fn",
    detail: "panic(msg)",
    doc: "prints panic: msg to stderr and exits with code 101",
  },
  {
    label: "size_of",
    kind: "fn",
    detail: "size_of[T]() -> usize",
    doc: "size of the type T in bytes",
  },
];

// Prelude surface the wasm32-wasi build actually embeds (boot/prelude/
// core.rho — the wasi tail only adds __-prefixed hooks user code never
// calls). Signatures are copied from core.rho.
export const PRELUDE = [
  {
    label: "cat",
    kind: "fn",
    detail: "cat(a: string, b: string) -> string",
    doc: "concatenates two strings into a fresh buffer",
  },
  {
    label: "assert",
    kind: "fn",
    detail: "assert(cond: bool)",
    doc: "panics with `assertion failed` when cond is false",
  },
  {
    label: "assert_eq",
    kind: "fn",
    detail: "assert_eq[T: Show](a: T, b: T)",
    doc: "panics with both values when a != b (needs Show)",
  },
  {
    label: "Option",
    kind: "enum",
    detail: "enum Option[T] { Some(T), None }",
    doc: "optional value; methods is_some() / is_none()",
  },
  {
    label: "Result",
    kind: "enum",
    detail: "enum Result[T, E] { Ok(T), Err(E) }",
    doc: "recoverable error; methods is_ok() / is_err(); unwrap with ?",
  },
  {
    label: "Show",
    kind: "trait",
    detail: "trait Show { fn to_str(self) -> string }",
    doc: "printable protocol — a type joins by defining to_str",
  },
];

// The static source as one flat list. Order matters only within one
// match tier: the rank field breaks ties, and every static rank sits above
// the rank 0 that scanSymbols emits for document symbols — locals from the
// buffer always outrank these (see completion-core.js filterItems).
export const STATIC_ITEMS = [
  ...BUILTINS.map((it) => ({ ...it, rank: 1 })),
  ...PRELUDE.map((it) => ({ ...it, rank: 2 })),
  ...PRIMITIVE_TYPES.map((it) => ({ ...it, rank: 3 })),
  ...KEYWORDS.map((it) => ({ ...it, rank: 4 })),
];
