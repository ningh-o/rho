# rho syntax

The surface grammar — the current law, carried by the self-hosted
compiler (`libs/compiler`); the frozen boot seed keeps its era's grammar
(slash `use`, no build parameters). Semantics live in
[type-system.md](type-system.md); program composition lives in
[module-system.md](module-system.md). Canonical formatting (`rho fmt`)
emits exactly the spacing shown here.

## Lexical structure

Source files are UTF-8, extension `.rho`. Whitespace separates tokens.
Comments run from `//` to end of line and ride the token that follows them
through `fmt`.

Keywords:

```
fn let mut if else while loop break continue return defer
struct enum use pub static const match as new null true false weak self
extern
```

Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Reserved: the keywords above
(`trait`, `impl`, `for`, and `dyn` included), the primitive type names (§2
of the spec), and the builtin names (`make`, `len`, `panic`, `printf`,
`eprintf`, `format`, `size_of`).

Integer literals: decimal (`12345`), hex (`0xFF`), binary (`0b1010`), with
`_` separators (`1_000_000`). An unsuffixed integer literal has no fixed
type; it adapts to the expected integer type in context and defaults to
`i32`.

Float literals: `3.14`, `1e9`, `2.5e-3`. No fixed type; adapts in context,
defaults to `f64`.

String literals: `"..."` with escapes `\n \t \r \\ \" \0 \xNN`. The lexer
decodes escapes, so the token's bytes are the string's bytes. String
literals are immutable static data.

Multiline string literals: `"""` opens a triple-quoted literal that runs
to the next unescaped `"""`:

```
let s: string = """
  hello
  world
  """;
```

- The literal is fully literal: every byte between the opening `"""` and
  the closing `"""` enters the value unchanged. The newline immediately
  after the opening `"""` is content like any byte, and so is the
  indentation — the example's value is `"\n  hello\n  world\n  "`.
- Escapes decode exactly as in single-line literals: `\n` puts a newline
  into the value without ending the source line, `\"` embeds a quote.
- An unescaped `"""` closes the literal — anywhere, mid-line included.
  To embed three quotes in the value, escape any one quote of the run:
  `\"""` decodes to `"""`.
- An unterminated literal (end of file before a closing `"""`) is a
  compile error.

`rho fmt` reprints such a literal canonically: the value's own bytes
between the delimiters — opener where the expression sits, the value's
newlines drawing the block's lines, its indentation the block's indent,
and its final line (one indent level of spaces) carrying the closing
delimiter. A value takes that form only when its own shape fits the
enclosing indent; anything else — no newline, a first byte other than
the opener's newline, interior lines indented shallower than the block,
a last line that is not exactly the block's indent — stays single-line
escaped, a form that always reparses to the same value byte for byte.

Operators and punctuation:

```
+ - * / %  == != < > <= >=  && || !  & | ^ << >>  ~
= += -= *= /= %= &= |= ^= <<= >>=
-> => ? . , : ; ( ) { } [ ]
..   (slice range)
...  (variadic parameter / spread argument)
```

## Declarations

```
fn name(a: T, b: U) -> R { ... }      // function
fn Recv.method(self: *Recv, x: T) { } // method; `self` first parameter
fn Type.make(x: T) -> *Type { ... }   // associated function
fn f[T](x: T) -> T { ... }            // generic function
fn f(a: i32, rest: i64...) { }        // variadic: last parameter only
struct Name { field: T, ... }         // struct
struct Pair[A, B] { a: A, b: B }      // generic struct
enum Shape { Circle(f64), Rect { w: f64, h: f64 }, Unit }
use io.file;                          // import; binds the namespace `file`
use net.http as http;                 // aliased import; binds `http`
pub use io.file;                      // re-export (the facade pattern)
pub ...                               // exported item (any declaration)
static NAME: T = expr;                // mutable module static
const NAME: T = expr;                 // compile-time constant
extern fn write(fd: i32, buf: *u8, n: usize) -> isize;
```

A variadic parameter (`rest: T...`) must be the **last** parameter of a
named `fn` declaration. Inside the body it is a `[]T` slice. `extern`
declarations and closure literals take no variadic parameters — the
function-value type has no spelling for "and then more".

### Build parameters (root consts)

A `const` declared in the entry (root) file **is a build parameter** — no
annotation, no keyword, nothing new to learn:

```
const native: bool = false;    // the root file's consts are build params
const level: i32 = 1;
const label: string = "web";
```

- Types are limited to `bool`, `i32`, and `string` literals; the
  initializer must be comptime (the const law already demands that).
- The name is visible in **every module** without a `use` — the same
  implicit status the prelude carries (`spec/module-system.md` §
  "Root build parameters").
- `rho build --set name=value` (repeatable) overrides the declared
  default by name: `rho build app.rho --set native=true`. An unknown
  name or a value that does not fit the declared annotation is a clear
  CLI refusal (exit 2).
- A module-level item named like a build parameter is an error
  (`native` shadows a build parameter declared in the root file) — a
  silent shadow would change what every `if (param)` in that module
  folds to.
- Artifact symbols are not part of the language surface: internal
  symbols compile to the shortest serial names `a..z, aa..` (nothing
  outside the artifact can call them by name), and `rho build -g` keeps
  the full `rho_<mod-index>__<name>` spellings for debugging dumps and
  panic stamps.

## Statements

```
let x: T = expr;        // immutable binding; type optional when inferable
let mut y: T = expr;    // mutable binding
x = expr;  x += expr;   // assignment (statement, not expression)
expr;                   // expression statement
if ...                  // § control flow
while c { ... }         // loop while c
loop { ... }            // infinite loop; exit via break/return
break;  continue;       // innermost loop
return expr?;           // must be last statement of a block path
defer stmt_or_block;    // runs at end of enclosing block scope, LIFO
{ ... }                 // nested block scope
```

`let` requires an initializer. Shadowing an outer name is an error.

### Expressions vs statements

`if` and `match` are expressions. A block's value is its final expression
(no trailing `;`). `let x = if c { 1 } else { 2 };` — both arms must have
the same type. Assignment is a statement; `a = b` has no value.

## Expressions

### Precedence (loosest to tightest)

| Level | Operators |
|-------|-----------|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `==` `!=` |
| 4 | `<` `>` `<=` `>=` |
| 5 | `\|` |
| 6 | `^` |
| 7 | `&` |
| 8 | `<<` `>>` |
| 9 | `+` `-` |
| 10 | `*` `/` `%` |
| 11 | unary `!` `~` `-` `*` (deref) `?` |
| 12 | postfix: call `f(x)`, index `a[i]`, field `a.b`, method `a.m(x)`, slice `a[i..j]`, `as` cast |

All binary operators are left-associative. `&&`/`||` short-circuit. On two
strings `+` concatenates (type-system.md § operators); there is no implicit
string conversion anywhere — a non-string operand on either side of `+` is
a type error, not a silent `to_str`.

### Postfix forms

```
f(a, b)            call; same syntax for static fns and closure values
sum(1, 2, 3)       variadic call — extra arguments fill `rest: T...`
sum(xs...)         spread call — passes the slice `xs` as `rest` whole
a[i]               index; bounds-checked, panics with index and length
a[i..j]            slice: i <= j <= len, bounds-checked, shares the buffer
p.b                field; pointer auto-deref, any depth
obj.m(args)        method call; sugar for Type::m(obj, args)
mod.item           module item
Type.make(..)      associated function
Enum.Variant(..)   variant construction (tuple/struct/unit)
x?                 error-path sugar (spec §8)
5.to_str()         untyped literals take their default width (i32/f64)
```

A spread (`xs...`) must be the **last** argument of a call and fills a
variadic parameter; using it against an ordinary parameter is an error.

### Allocation, casts, closures

```
let p: *Point = new Point { x: 1, y: 2 };   // heap struct, rc = 1
let s: []i32  = make([]i32, 16);            // zeroed heap array, rc = 1
x as i32                                    // cast (spec §4.2 rules)
fn(a: i32) -> i32 { return a + 1; }         // closure literal
```

## Control flow

```
if c { } else if c2 { } else { }
match value {
    Circle(r)      => draw(r),
    Rect { w, h }  => draw(w * h),
    Point          => 0,
    4              => four(),
    _              => panic("unknown"),
}
```

Conditions are `bool` — no truthiness. The `if` expression form requires an
`else`. `break`/`continue` target the innermost loop. `defer` runs on every
exit path of its scope, including `break`/`continue`/`return` and panics.

### Comptime-folded conditions

A condition the compiler can decide at compile time — a literal, a const
(build parameters included), or a comptime combination of those with `&&`,
`||`, `!`, comparisons, and integer arithmetic — folds the `if` at check
time: only the live branch is checked and lowered. The dead branch is
parsed and then skipped whole: names it alone references never resolve
(calling a function that does not exist inside a dead branch is not an
error), its diagnostics never fire, and modules only it reaches are
dropped from the artifact by reachability. That is how one source ships
two configurations — `native` code lives inside `if (native)`, and a
`--set native=false` build compiles as if it were not there.

Match arms are `pattern => expr,` and may open a block. Patterns: unit /
tuple / struct variants of an enum (with `_` placeholders or bindings),
`_` wildcard, integer and string literals. Bindings bind by copy. Match is
an expression; all arms must produce the same type, and exhaustiveness is
checked unless `_` is present.
