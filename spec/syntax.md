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
fn let mut const static extern struct enum trait impl for dyn
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
type      := builtin | ptr | opt | slice | fn_type | path | app
builtin   := i8|i16|i32|i64|u8|u16|u32|u64|usize|f32|f64|bool|string
ptr       := '*' type            (*T — non-null heap pointer)
opt       := '?' type            (?T — sugar for Option[T])
slice     := '[' type ']'        ([T] — slice of T)
fn_type   := 'fn' '(' [params] ')' ['->' type]
path      := ident ['.' ident]   (a struct/enum/trait name, or Option/Result)
app       := path '[' types ']'  (generic instantiation: Pair[i32, f64])
```

`?T` parses as `Option[T]` before type checking sees it; there is one
absence mechanism. `usize` is the unsigned word type (u32 on wasm32) and
the type of `len` results and index expressions.

## 4. Declarations

A module is a sequence of declarations; order within a module is free.

```
decl      := fndecl | struct | enum | trait | impl | const | static
           | extern | use | pub_use
```

### 4.1 Functions

```
fndecl    := 'fn' name generics? '(' params ')' ['->' type] block
generics  := '[' ident (',' ident)* ']' ([':' bounds]? — see below)
bounds    := bound (',' bound)*
bound     := ident               (a trait name)
params    := param (',' param)*
param     := 'mut'? ident ':' type ['...']    ('...' marks variadic)
```

- The last parameter may be variadic: `rest: T...`. It is a concrete
  element type `T`, seen as `[]T` in the body. A variadic parameter
  must be last; at most one per function; variadic functions are not
  first-class values.
- A function whose first parameter is `self: *T` (or `self: T`) is a
  **method** of `T` and must be named `fn T.name(...)`:
  `fn Rect.area(self: *Rect) -> i32`. A function named `fn T.name`
  whose first parameter is not `self` is an **associated function**:
  `fn Rect.square(n: i32) -> *Rect`, called as `Rect.square(3)`.
- Overloads: several functions may share one name in a module (and
  across modules — see `type-system.md` §5). Resolution is exact-match
  unique.
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
use       := 'use' segs ';'
segs      := ident ('.' ident)*
pub_use   := 'pub' 'use' useform ';'
useform   := segs                       (re-export a module/package)
           | segs '.' ident             (re-export one item)
           | segs '.' ident 'as' ident  (re-export one item, renamed)
           | segs '.' '*'               (flatten all public items)
```

Plain `use` has exactly one form: dotted path, segments are plain
identifiers. No `as`, no `*` on plain use. Use bindings are private to
the importing module. `pub use` is legal only in a facade (`lib.rho`)
and has exactly the four forms above (see `module-system.md`).

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
- `defer expr;` — the expression (a call) runs when the enclosing scope
  exits, LIFO across defers, on **every** exit path except panic.
- Compound assignment is defined for every arithmetic/bitwise operator
  (`<<`/`>>` included) on mutable lvalues of matching type; both sides
  same type; bitwise compound assignments are verified end-to-end.

## 6. Expressions

### 6.1 Precedence (C-style, 11 levels + postfix)

Highest to lowest, all binary levels left-associative:

| level | operators            |
| ----- | -------------------- |
| 11    | postfix: call, field, index, `?` |
| 10    | unary `-` `!`        |
| 9     | `*` `/` `%`          |
| 8     | `+` `-`              |
| 7     | `<<` `>>`            |
| 6     | `&` `|` `^`          |
| 5     | `==` `!=` `<` `<=` `>` `>=` |
| 4     | `&&`                 |
| 3     | `\|\|`               |
| 2     | `as`                 |
| 1     | everything else (assignments are statements; match/if/ closures are primary expressions) |

`&&` and `||` short-circuit. Conditions of `if`/`while` are `bool`;
there is no truthiness.

### 6.2 Primary expressions

- literals (§2), paths (`a` or `a.b` — module items and enum
  constructors), calls `f(args)`, method calls `x.m(args)`,
  associated calls `T.m(args)`
- field access `p.x`, indexing `s[i]` (index type `usize`)
- parenthesized `(e)`
- `new` expressions: `new T { f: e, ... }`, `new T(args)` (enum
  variants), with explicit generics `new Pair[i32, i64] { … }`
- slice literals `[a, b, c]` (type `[]T`), empty `[]` with consumer
- `if`/`match` expressions (same-type arms; `if` as an expression
  requires `else`)
- closures (§6.8), `?` (§6.9), `as` casts (§6.7)

### 6.7 `as`

`e as T` — the only conversion (see `type-system.md` §3 for the full
matrix). Binds tighter than comparisons, looser than `||`.

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
```

- `ident` binds; the same binder twice in one pattern is an error;
  `_` matches anything.
- Enum variants appear by full path (`Shape.Circle(r)`), tuple or
  struct form matching the declaration.
- Literal patterns: integers, floats, bools, strings.
- Match arms: `pattern => expr` or `pattern => block`. Arms are
  expressions of the same type; a match used as a statement may have
  unit arms of differing statement shapes only via blocks.
- Exhaustive unless `_` is present: a match on an enum must cover every
  variant or carry `_`.

## 8. Formatting (fmt)

`rho fmt` prints the canonical form: 2-space indent, one statement per
line, trailing struct/enum commas, normalized spacing per this
document. `rho fmt` then re-parse is byte-for-byte identity — fmt is
the canonical pretty-printer, and its output is a fixed point.
