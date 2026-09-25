# rho syntax — the law

This document is the complete syntactic law of rho 0.1.0. It is derived
from the ratified design (TODO.md, "The design"). Where the design names
a rule, this document makes it concrete; nothing here may exceed the
design. Implementation disagreements are resolved in the design's favor.

## 1. Source files

- A source file is UTF-8 text. A file ends with an optional final
  newline; the compiler accepts any mix of `\n` and `\r\n` line endings
  and normalizes them to `\n` internally.
- One file is one module (see `module-system.md`). The file stem is the
  module name; `main.rho` is the entry file of a program.
- Comments: line comments start with `//` and run to end of line.
  There are no block comments.

## 2. Lexical grammar

### 2.1 Identifiers and keywords

```
ident    := [a-zA-Z_][a-zA-Z0-9_]*
```

Keywords (reserved in every position they could collide):

```
fn let mut const static extern struct enum trait impl for dyn test
use pub as if else while loop match return defer break continue
new null true false
i8 i16 i32 i64 u8 u16 u32 u64 usize f32 f64 bool string
```

`null` is reserved **and removed from the language**: it is a lexical
keyword that never parses (pointers are non-null; absence is `?T`).
Reserved-for-future words are not held back; the list above is final
for 0.1.0.

### 2.2 Integer literals

```
int_lit  := dec | hex | bin
dec      := [0-9][0-9_]*
hex      := 0x[0-9a-fA-F_]+
bin      := 0b[01_]+
```

Underscores separate digits and carry no value. The value is a
non-negative mathematical integer; negation is the unary `-` operator,
not part of the literal. A literal whose value does not fit its
consumer type is a compile error; with no consumer an integer defaults
to `i32` (value must fit `i32`).

### 2.3 Float literals

```
float_lit := digits '.' digits [exponent] | digits exponent
exponent  := [eE] [+-]? digits
digits    := [0-9][0-9_]*
```

With no consumer a float defaults to `f64`. `1.` and `.5` are not float
literals (`1.` is `1` followed by `.`; field access on integers is not
a thing, so this is a syntax error in practice).

### 2.4 Boolean literals

`true` and `false`, of type `bool`.

### 2.5 String literals

Two forms.

**Single-line strings**: `"…"` with escapes. The escape set is exactly:

| escape    | value                          |
| --------- | ------------------------------ |
| `\n`      | 0x0A                           |
| `\r`      | 0x0D                           |
| `\t`      | 0x09                           |
| `\\`      | 0x5C                           |
| `\0`      | 0x00                           |
| `\"`      | 0x22                           |
| `\xHH`    | one byte, HH hex               |
| `\u{…}`   | UTF-8 encoding of one code point |

Any other escape is a compile error. A raw newline inside a single-line
string is a compile error. Strings are UTF-8 byte slices (§ types).

**Triple-quoted strings**: `"""…"""` are **fully verbatim**. Every byte
between the opening `"""` and the first subsequent `"""` is part of the
value, exactly as written: no escape sequences decode, no indentation is
stripped, the newline immediately after the opening `"""` is included,
and content may run right up to the closing `"""` mid-line. A `\""` or
`""` sequence inside is literal text (only a full `"""` terminates). A
triple-quoted string that never closes is a compile error.

## 3. Types

```
type      := builtin | ptr | opt | slice | fn_type | app | dyn
builtin   := i8|i16|i32|i64|u8|u16|u32|u64|usize|f32|f64|bool|string
ptr       := '*' type            (*T — non-null heap pointer)
opt       := '?' type            (?T — sugar for Option[T])
slice     := '[' ']' type        ([]T — slice of T, Go style)
fn_type   := 'fn' '(' [types] ')' ['->' type]   (unnamed param types)
app       := ident ['[' types ']']  (Pair[i32, f64]; bare ident = named type)
dyn       := 'dyn' ident            (trait object; the ident names the trait)
```

`?T` parses as `Option[T]` before type checking sees it; there is one
absence mechanism. `usize` is the unsigned word type — the address
width of its target (u32 on wasm32, the 0.1.0 backend; each Phase-7
backend pins its own) — and the type of `len` results and index
expressions.

## 4. Declarations

A module is a sequence of declarations; order within a module is free.

```
decl      := fndecl | struct | enum | trait | impl | const | static
           | extern | use | pub_use | test
```

### 4.1 Functions

```
fndecl    := 'fn' name generics? '(' params ')' ['->' type] block
generics  := '[' gparam (',' gparam)* ']'
gparam    := ident [':' bound (',' bound)*]   (see note)
bound     := ident               (a trait name)
params    := param (',' param)*
param     := 'mut'? ident ':' type ['...']    ('...' marks variadic)
```

**Bounds grammar ruling** (the design's `[T: Show, Eq]` made exact):
inside the generics bracket, once a `:` introduces a bound list, every
comma-separated identifier up to the closing `]` is a bound of that
parameter — a comma never returns to parameter position. `[A, B: Eq]`
declares two parameters; `[T: Show, Eq]` declares one parameter with
two bounds; `[A: Show, B]` declares one parameter with bounds `Show`
and `B` (a checker error when `B` is not a trait). Bounded parameters
therefore come last when several parameters are declared.

- The last parameter may be variadic: `rest: T...`. It is a concrete
  element type `T`, seen as `[]T` in the body. A variadic parameter
  must be last; at most one per function; variadic functions are not
  first-class values.
- A function whose first parameter is `self: *T` (or `self: T`) is a
  **method** of `T` and must be named `fn T.name(...)`:
  `fn Rect.area(self: *Rect) -> i32`. A function named `fn T.name`
  whose first parameter is not `self` is an **associated function**:
  `fn Rect.square(n: i32) -> *Rect`, called as `Rect.square(3)`.
  In impl methods the receiver may be written bare — `self`
  (read-only `*T`) or `mut self` (writable) — in both homes: inside an
  impl block the type comes from the header (`impl Show for Pt {
  fn to_str(self) -> string }`), in a dotted method from the name
  (`fn Pt.to_str(self)`); the fully-typed form stays legal.
- Overloads: several functions may share one name in a module (and
  across modules — see `type-system.md` §5). Resolution is exact-match
  unique.
- `mut` sits before the parameter name and is legal only on
  handle-typed parameters (`*T`, `[]T`, `string`, `dyn`): it grants
  write access to the shared view. Value parameters are immutable —
  the body rebinds with `let mut p = p;` when it must mutate its
  copy. The full view law is the design (TODO.md §18).
- Closures are anonymous function expressions (§6.8).

### 4.2 Structs, enums, traits, impls

```
struct    := 'struct' name generics? '{' fields '}'
fields    := field (',' field)* ','?
field     := ident ':' type
enum      := 'enum' name generics? '{' variants '}'
variants  := variant (',' variant)* ','?
variant   := name | name '(' types ')' | name '{' fields '}'
trait     := 'trait' name '{' sigs '}'
sigs      := sig (',' sig)* ','?
sig       := 'fn' name '(' params ')' ['->' type]
impl      := 'impl' path 'for' type '{' fndecl* '}'
```

- Trait signatures use the method form: `fn to_str(self) -> string` —
  the implementing function is `fn Pt.to_str(self: *Pt) -> string`.
- Multiple impl blocks per (trait, type) are allowed; impls may live in
  any module (the old ownership rule is gone; coherence is enforced at
  call sites by exact-match-unique).

### 4.3 Constants, statics, externs

```
const     := 'const' name ':' type '=' expr ';'          (annotated)
           | 'const' name '=' expr ';'                   (inferred)
static    := 'static' 'mut' name ':' type '=' expr ';'
extern    := 'extern' name ':' fn_type ';'
```

- `const` is immutable, folded at compile time. The annotation may pin
  any builtin type; without annotation the type is inferred from the
  initializer.
- `static mut` is the **only** global state: module lifetime,
  mutable. Static initializers must be acyclic and comptime-evaluable
  (literals, consts, and plain constructors of them).
- Root-file consts are build parameters (`spec.md` §6).

### 4.4 use and pub use

```
use       := 'use' segs ['as' ident] ';'
           | 'use' segs '.' '{' items '}' ';'
segs      := ident ('.' ident)*
items     := item (',' item)* ','?
item      := ident ['as' ident]
pub_use   := 'pub' 'use' useform ';'
useform   := segs                       (re-export a module/package)
           | segs '.' ident             (re-export one item)
           | segs '.' ident 'as' ident  (re-export one item, renamed)
           | segs '.' '*'               (flatten all public items)
```

A plain `use` binds one name two ways: if the final segment resolves
to a module (file or directory), the binding is the module — access
stays qualified (`lex.token`); if it resolves to a public item of the
module it lands in, the binding is the item itself, used unqualified,
under its own name or the `as` name. The brace form is pure sugar: it
expands to one plain `use` per item. Resolution order, the package
boundary, and the collision law are `module-system.md` §2/§4.
`pub use` is legal only in a facade (`lib.rho`) and has exactly the
four forms above; it keeps the re-export monopoly (see
`module-system.md` §6).

### 4.5 Test blocks

```
test      := 'test' string block
```

A top-level `test "name" { … }` declares one test: a synthesized void
fn in its own module — white-box, it sees the module's private items —
judged by panic (assert) versus clean return. Test blocks are checked
in every mode and emitted only under the `rho test` verb; the full
protocol (file tests, golden headers, the runner's law) lives in the
design (TODO.md §17).

## 5. Statements

```
block     := '{' stmt* '}'
stmt      := letstmt | assign | if | while | loop | match | return
           | defer | 'break' [ident] ';' | 'continue' [ident] ';'
           | expr ';'
letstmt   := 'let' ['mut'] ident ':' type '=' expr ';'
           | 'let' ['mut'] ident '=' expr ';'
assign    := lvalue ('=' | '+=' | '-=' | '*=' | '/=' | '%='
           | '&=' | '|=' | '^=' | '<<=' | '>>=') expr ';'
lvalue    := ident | lvalue '.' ident | lvalue '[' expr ']'
while     := [ident ':'] 'while' expr block
loop      := [ident ':'] 'loop' block
```

- `let` requires an initializer. The annotation may be omitted when the
  initializer's type is determined without it (a consumer-typed literal
  uses the annotation as its consumer; `let x = 3;` types `x` as `i32`,
  the default).
- **Inner shadowing is allowed**: a `let` in an inner scope may rebind a
  name from an outer scope. Two `let`s of the same name in the *same*
  scope are an error. A `let` may not shadow a root build parameter.
- Assignment is a statement, not an expression; it has no value.
- Labels: a plain identifier + `:` on `while`/`loop`; `break L;` /
  `continue L;` jump out of / re-enter the labeled loop. Labels are
  function-unique and live in their own namespace (a label may share a
  name with a variable). No goto.
- `defer expr;` or `defer lvalue op= expr;` — the deferred action (a
  call or an assignment) runs when the enclosing scope exits, LIFO
  across defers, on **every** exit path except panic.
- Compound assignment is defined for every arithmetic/bitwise operator
  (`<<`/`>>` included) on mutable lvalues of matching type; both sides
  same type; bitwise compound assignments are verified end-to-end.

## 6. Expressions

### 6.1 Precedence (C-style, 11 levels + postfix)

Highest to lowest, all binary levels left-associative (exactly C's
ladder, with `as` inserted Rust-style above the multiplicative level):

| level | operators            |
| ----- | -------------------- |
| 11    | postfix: call, field, index, slice, `?` |
| 10    | unary `-` `!` `*` `~`  |
| 9     | `as`                 |
| 8     | `*` `/` `%`          |
| 7     | `+` `-`              |
| 6     | `<<` `>>`            |
| 5     | `<` `<=` `>` `>=`    |
| 4     | `==` `!=`            |
| 3     | `&`                  |
| 2     | `^`                  |
| 1     | `\|`                 |
| 0     | `&&` then `\|\|` (&& binds tighter) |

`&&` and `||` short-circuit. Conditions of `if`/`while` are `bool`;
there is no truthiness.

### 6.2 Primary expressions

- literals (§2), paths (`a` or `a.b` — module items and enum
  constructors), calls `f(args)` — an argument may be prefixed `mut`
  when, and only when, the callee's parameter is a `mut` view, and
  the argument's own binding must be `mut` (TODO.md §18), method
  calls `x.m(args)`,
  associated calls `T.m(args)`
- field access `p.x`, indexing `s[i]` (index type `usize`)
- slicing `s[a..b]` with either end open (`s[..n]`, `s[n..]`)
- parenthesized `(e)`
- `new T { f: e, ... }` with optional explicit generics
  (`new Pair[i32, i64] { … }`); `*new T { … }` copies the value inline
- unary `*p` dereferences a pointer (yields a copy of the pointee
  value); unary `~` is integer bitwise-not
- `make([]T, n)` — the builtin zeroed-slice allocation (n elements of
  type T); `make` is syntax, not a function
- enum variant constructors by path: `Shape.Circle(r)`,
  `Shape.Rect(w: 7, h: 3)` — tuple position or named fields
- slice literals `[a, b, c]` (type `[]T`), empty `[]` with consumer
- `if`/`match` expressions (same-type arms; `if` as an expression
  requires `else`)
- closures (§6.8), `?` (§6.9), `as` casts (§6.7)

### 6.7 `as`

`e as T` — the only conversion (see `type-system.md` §3 for the full
matrix). Level 9 exactly: looser than unary (`-x as T` reads
`(-x) as T`), tighter than every binary operator below it.

### 6.8 Closures and function values

```
closure   := 'fn' '(' params ')' ['->' type] block
```

Non-variadic functions and closures are first-class values of `fn`
type. Closures capture immutable locals **by copy**; capturing a `mut`
local is a compile error (shared mutable state is a heap object).

### 6.9 `?`

`e?` where `e: Option[T]` or `e: Result[T, E]` — propagates absence
(`None` / `Err(e)`) out of the enclosing function; the expression's
value is `T`. The enclosing function's return type must be an
`Option`/`Result` compatible with the propagated value
(`type-system.md` §12).

## 7. Patterns

```
pattern   := literal | ident | '_' | variant | struct_pat
variant   := path '(' patterns? ')' | path '{' binders? '}'
binders   := ident (',' ident)* ','?
struct_pat := path '{' ident ':' pattern (',' …)* ','? '}'
literal   := int | float | bool | string (single-line)
arm       := arm_pat ['if' expr] '=>' (expr | block)
arm_pat   := pattern ('|' pattern)*
```

- `ident` binds; the same binder twice in one pattern is an error;
  `_` matches anything.
- Enum variants appear by full path (`Shape.Circle(r)`), tuple or
  struct form matching the declaration — or bare by the scrutinee's
  type: a variant name in an arm resolves against the enum being
  matched first (`Some(v)`, `Circle(r)`; prelude and user enums
  alike), with no scope fallback. Full paths stay legal and are
  required for variants of any other enum.
- Literal patterns: integers, floats, bools, strings.
- Match arms: `arm_pat => expr` or `arm_pat => block`; alternatives
  of an or-pattern bind the identical name set, else a compile
  error. An optional guard — `arm_pat if cond` — evaluates after the
  pattern matches, in scope of its bindings. Arms are expressions of
  the same type; a match used as a statement may have unit arms of
  differing statement shapes only via blocks.
- Exhaustive unless `_` is present: a match on an enum must cover every
  variant or carry `_`; guarded arms never close exhaustiveness.

## 8. Formatting (fmt)

`rho fmt` prints the canonical form: 2-space indent, one statement per
line, trailing struct/enum commas, normalized spacing per this
document. `rho fmt` then re-parse is byte-for-byte identity — fmt is
the canonical pretty-printer, and its output is a fixed point.
