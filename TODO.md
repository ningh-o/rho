# rho — restart to 0.1.0

> **First action: back the current branch up to the remote archive.** See
> T0.1. Nothing happens before that copy is verified on the remote.

The language design in this file is **final** — ratified item by item on
2026-09-25. Implementation starts from zero: a fresh git history with no
carried-over commits, a fresh compiler, and exactly one release version:
**0.1.0**. There are no intermediate version numbers and no version
evolution — everything before the 0.1.0 gate is unversioned work on the
road to it. Do not invent versions; do not bump; do not tag anything else.

**0.1.0 scope: the wasm self-hosting bootstrap only.** boot (C, wasm
backend) → the self-hosted compiler written in rho → it compiles itself
to wasm → gate green → 0.1.0. Native compilation is **not** part of
0.1.0: it moves into the standard-library workstream after 0.1.0
(Phase 7).

## Working protocol (every phase, no exceptions)

- When a phase starts, expand it into a small TODO list **in this file**.
  Each small TODO is exactly **one commit**: English commit message,
  prefixed with the TODO id (`T3.4: checker — overload resolution`).
- **Every commit carries its tests, and they pass.** A commit without its
  tests does not go in; a red test blocks the commit. **No leftover
  issues, no skipping** — no skipped/ignored/known-failure markers
  anywhere in the tree, ever. A problem is either fixed in the commit
  that triggers it, or the commit does not happen.
- A completed TODO is marked `[x]` in the commit that completes it (or in
  the next commit at the latest). Never mark ahead of the work.
- Every run is time-capped (perl alarm / poll pattern; never an
  unbounded wait).
- Standing laws: every path that overwrites an executable writes to a
  temp file and renames; browser/GUI walkthroughs assert **output panel
  content**, never button recovery; verification runs at the **default
  wasm stack** (a big-stack run proves nothing).
- **The archive branch is reference-only.** Reading it, comparing
  approaches, and studying its bug ledgers are allowed and encouraged;
  **copying code from it is forbidden.** Corpus *programs* (test data)
  may be adapted; implementation code may not.
- The design section below is **the law**. When implementation and design
  disagree, the implementation is wrong. If the design itself must
  change, change it here first, in a commit of its own, with the reason
  written.

---

## Phase 0 — archive, then a fresh start

- [x] **T0.1 Back up the current branch to the remote archive.** Commit
      the outstanding working-tree changes (site editor work, the visual
      probe script) onto a local `archive/pre-0.1.0` branch, push it:
      `git push origin archive/pre-0.1.0`. Verify the remote branch
      contains the final old-implementation state (HEAD b348133 plus the
      working tree). This is the only lifeline for all prior material —
      verify before touching anything else.
- [x] **T0.2 Fresh history.** Recreate `master` from an orphan branch:
      no commits carried over. The fresh tree holds only this TODO.md, a
      README stub, and LICENSE (MIT). Everything else — the old
      compiler, corpus, spec, site, tools — lives on the archive branch:
      **reference-only; copying code is forbidden.**
- [x] **T0.3 Rewrite the spec (the law).** From the design section below,
      write the four documents fresh: `spec/syntax.md`,
      `spec/type-system.md`, `spec/module-system.md`, `spec/spec.md`
      (memory model, compilation model, determinism, version policy —
      version policy says: 0.1.0 = the wasm self-hosting bootstrap, or
      nothing). Add a conformance map: every design rule → the test that
      will hold it.
- [x] **T0.4 Corpus plan.** Decide the corpus rebuild from the archive:
      which of the 105 old programs adapt to the new language and in
      what order, which new programs the new features need (overloads,
      `?T`, labels, inner shadowing, build-parameter widening). Goldens
      are behavioral (stdout + exit) and will be regenerated from the
      new seed once it exists — never hand-written.

## Phase 1 — the C seed implements the complete design

boot is rewritten in C: **the only backend in 0.1.0 is wasm32-wasi**;
arena-allocated, one-shot, implementing **every** rule in the design
section — no placeholder stages. boot is the reference compiler.

- [x] **T1.1** Skeleton: build, arena, selftest harness, AST dump.
- [x] **T1.2** Lexer: full grammar — keywords incl. `trait impl for dyn`,
      integer/float literals (default lanes i32/f64), string escapes,
      **fully verbatim triple-quoted strings**, dotted `use`, labels,
      `?T` notation.
- [x] **T1.3** Parser: declarations (fn/methods/associated/generic,
      struct, enum, trait, impl, const/static/extern, use/as/pub use),
      overloads (same name, many signatures), patterns, labels on
      `while`/`loop`.
- [x] **T1.4** Checker I — names: locals → module → root build params →
      prelude; **inner shadowing allowed** (same-scope rebind is still an
      error); two visibility tiers + package facades; method candidate
      sets = native methods (with their type) ∪ methods of modules in the
      use closure; same-signature ambiguity = compile error naming both
      modules.
- [x] **T1.5** Checker II — types: the eleven type-system rules
      (consumer-typed literals with fixed defaults; const inference with
      optional builtin annotations; `as`-only conversions with unified
      truncating semantics on constants and variables; overload
      resolution = exact match unique; trait satisfaction = name +
      signature match; `[T: Bound]` bounds verified per instantiation;
      **non-null pointers by default, `?T` = Option sugar**, no smart
      casts; element-wise `==` behind the comparability law).
- [x] **T1.6** Checker III — errors and folding: `?` on Result/Option;
      panic law; comptime folding of root-build-parameter conditions
      (dead branch parsed then skipped whole; reachability prunes
      modules); `--set name=value` with the **widened type face** (bool,
      all integer widths, f32/f64, string; range-checked; refusal = exit
      2).
- [x] **T1.7a** Lower + emit core: the
      WAT pipeline (wat2wasm pinned, tmp+rename law), kernel.wat
      (allocator/rc glue/decimal print/wasi tail), scalar types,
      literals, arithmetic (wrap/narrow/shift-mask/div-panic), calls
      with the chosen-overload annotation, printf desugar (ints, bools,
      strings), consts/statics, `_start`. hello→wasmtime green.
- [x] **T1.7b** structs + *T — boxed layout, field access through
      pointers, methods, `new` with the zeroed/rc law, drop-fn registry,
      managed-value moves (retain/release discipline), defer LIFO on
      every exit (incl. continue/break), assignment (simple + compound
      + through pointers).
- [x] **T1.7c** enums + match (tag dispatch, patterns/binders,
      exhaustiveness + wildcard gating), Option/Result + `?`
      propagation, `make`/slices/len/index/`a..b`, string concat/`==`
      content.
- [x] **T1.7d** labels lowering, TCO (self tail call → loop,
      million-deep recursion verified), if/while/loop statements,
      if/match as expressions (escape ownership retained).
- [x] **T1.7e** closures (capture boxes) ✅, fn values (trampolines) ✅,
      generic fn monomorphization (bind/clone/emit) ✅, associated-fn
      calls `Type.name(args)` ✅, user-defined to_str printing ✅ (§8),
      generic struct/enum instantiation ✅ (field types through the
      binds; dense field offsets at the BOUND width), methods on
      generic types ✅ (the type's params ride implicitly, bound from
      each receiver), trait bounds verified per instantiation ✅
      (concrete-over-generic overload precedence), dyn + vtables ✅
      (fat {vtable, obj}; per-(trait,type) shim runs in one append-only
      funcref region; coercion at every expected-type site; dispatch
      through the closure ABI; 084–089 land byte-exact).
- [x] **T1.7** (completes when a-e cover the conformance map rows) rc insertion per the pure-local
      counting rules ✅ (incl. enum-payload deep retain/release, let/assign
      copy retain), zeroed allocations ✅, container ownership walk at the
      rc==1 death check, tail-call→loop ✅, labels lowering ✅.
- [x] **T1.8** Kernel prelude (the whole kernel, nothing more):
      Option/Result + `?` machinery ✅ (None rides tag 0 so zeroed
      make/new read as the absent form), `to_str` family + variadic
      format sinks ✅ (every primitive incl. the exact-decimal float
      text; printf/eprintf/format desugar; the Show trait + generic
      assert_eq), panic/assert + hooks ✅, allocator + rc glue ✅
      (rho_alloc/retain/release + weak), string primitives ✅
      (rho_cat2/rho_streq), wasi raw-syscall tail ✅ (fd_write +
      proc_exit — the two every current mechanism needs; file io and
      args grow with std.io / the Phase-2 CLI, per the
      kernel-grows-only-with-a-mechanism law). `read_line` correctly
      absent (std.io's job, Phase 4). Audited 2026-09-25: mechanism
      set complete, nothing extra.
- [x] **T1.9** CLI: build/run/fmt/check ✅ (`-g` routes to the
      emitter); `rho test` delegates to the shell runners (selftest +
      check + emit + fmt + corpus; the in-binary form lands with the
      gate); fmt canonical roundtrip green (27/27).
      THE COMMENT LAW (2026-09-27): fmt may not drop or edit a
      comment — test headers (`// out:`, `// spec:`) and every other
      comment replay verbatim. The lexer records comments in a side
      table (they never become tokens); the parser stamps each
      construct's end_line (stmt/decl wrappers, blocks, arms, struct
      fields, enum variants and payload fields, trait sigs, impl
      members); fmt walks a monotonic cursor — a comment on the
      construct's own last line rides as its trailing note, every
      other comment leads the next construct at the current indent
      (gofmt's law, sized for an AST printer). run-fmt-tests.sh now
      asserts the comment multiset survives (not just the fixpoint),
      tests/emit/fmt_comments.rho pins headers, fields, variants,
      arms, members, and file-tail comments, and the else-spelling
      wart (`} else  {`) died in both formatters. The mirror's fmt
      still strips comments (its fixtures are comment-free so parity
      holds) — the mirror grows the same law with its fmt parity
      breadth (T2.2's ledger).
- [x] **T1.10(tranches 1+2+3)** — 108 in-repo programs with goldens
      judged against the archive (n10 restored; floats/enum-payload/
      statics/to_str/null-family/assoc/MIN-division all landed
      byte-exact or lawfully regenerated; tranche 2 complete: 031
      verbatim, 032 facade tree, 102/105 []?T). Tranche 3 (t01–t12,
      prefixed t to sit beside the n-series): overloads, opt sugar,
      labels, as-const-var, aggregate let, pub-use forms (t09 with
      its pk/ package tree), verbatim, compound bitwise, precedence —
      plus tests/run-set-tests.sh pinning --set's widened face (the
      runner honors `// set:` header lines). The archive scan reads
      79 pass + 3 known law-diffs (in-repo regenerated) + 18
      adapted-in-repo + 107 (std.io, Phase 4) — full coverage.

## Phase 2 — the self-hosted compiler, written in rho (wasm only)

- [x] **T2.1** Compiler package skeleton (lex → parse → emit_wasm),
      compiled **by boot**; hello-world compiles itself
      (libs/compiler/{lex,parse,emit,main}.rho — tokens with raw-span
      strings, a fn-main/printf/return grammar, a self-contained
      wasm32-wasi emitter with a string pool; the source rides the
      SRC build parameter per §7; tests/run-selfhost.sh pins the
      loop: boot → rho compiler → hello → wasmtime). Graded growth
      (types, checker, fmt, the full parity surface) is T2.2+.
- [x] **T2.2+** Port module by module; each module its own TODO; graded
      by behavioral parity with boot across the whole corpus
      (determinism law: same compiler + same input → identical bytes).
      *(growth so far, each pinned by tests/run-selfhost.sh, the
      boot-vs-self differential, and — since the twenty-sixth growth —
      tests/run-corpus-diff.sh running the WHOLE corpus through both
      compilers (46 of 108 behavioral today, pinned floor 46): integer
      printing with a decimal runtime; string hole args; let bindings,
      arithmetic with precedence, variables; while/if/else-if/assignment
      with comparisons and unary minus; user fns with parameters, call
      statements, expression returns — recursion lands (fib(10)=55);
      string values with literal-concatenation folding; string
      parameters over a pair-passing ABI; len(); main's return as the
      exit code; the checker module (unknown fns/names/labels refuse
      emission); WAT streaming in slices; verbatim triple-quoted
      strings (the emitter's templates ride them); break/continue and
      loop; short-circuit &&/|| (the full precedence ladder renumbered
      to §6.1, call args fixed to parse it); labeled loops with
      unique wasm labels and a scoped label check; integer bitwise
      & | ^ << >> ~ with all compound forms; slices — make/index/
      slice-of/len with a zeroed bump allocator, bounds panics at
      exit 101, fat-view aliasing, and string byte indexing; module
      consts/statics (folding consts, wasm globals); defer with a
      frame stack (returns park the value in $r while scopes unwind
      LIFO; loop exits run their defers — 075's law); self tail calls
      become loops (a two-million-deep countdown rides the default
      wasm stack); narrow-width arithmetic fidelity (annotations kept,
      widths propagate through arithmetic with literals adapting and
      declaring operands winning, the i32 literal default, sub-32
      shifts on the 32-bit lane, unsigned shr_u, MIN/-1 division via
      if-select, unsigned printing through $w_u64); the full escape
      law (\r \0 \xHH \u{...}); hex and bin literals; /0 %0 panics;
      and the formatter — fmt.rho byte-identical to boot on the
      subset (tests/run-fmt-self.sh), with the parser retaining what
      fmt needs (mut, types, compound spellings, init exprs). The
      growths also fixed a real boot bug: --set overrides now land
      before the load-time dead-branch fold (they used to fold against
      the declared value while printing the override). Later growths: the assert
      builtin (eqz-gated panic; assert_eq compares strings content-wise
      with a user's same-name fn winning), runtime string concat
      ($w_cat + build_string_pair over literal/var/slice/call leaves,
      chained slices, loop rebinds binding slots BEFORE the tree
      builds), the string-return ABI, format() and $w_itoa, brace
      escapes in format strings, and len(base[a..b]) computed directly
      (bounds-checked, hi - lo, no view built). fmt parity breadth:
      31 of the 46 compile-passing corpus files also format
      byte-identically; the other 15 mark the parse-retention gaps
      (escape spellings, shapes) the subset fmt has yet to learn).
      The thirty-fifth through thirty-eighth cuts (2026-09-26) land the
      aggregate half of the language: structs (declarations over
      composed types, new with named fields, pointer/value bindings —
      (*p) copies into a local frame whose writes alias the shared
      storage, fn Type.name methods and associated fns), enums and
      match (heap-box values riding the i64 lane, unit/tuple/struct
      payloads, literal/variant/wildcard patterns over scalars or
      tags, block arms, string-valued matches as pair chains, enum
      equality through a $w_enuq runtime, as-T = the tag), and the
      Option/Result layer (the ? unwrap-and-return with defers, the
      shared zeroed None box filling fresh ?T slices, payload-typed
      binders, printf in expression position). The names-table
      lookups scan backward now — match binders shadow across arms.
      55 of 108 behavioral, floor 55. The thirty-ninth through
      forty-third cuts (2026-09-26, one session): stringly scalar
      lanes and printf's evaluation order (56); the f64 lane WITHOUT
      the printer — literals ride their raw text into f64.const
      (wat2wasm does the strtod), values live as bits in the i64
      locals with reinterpret round-trips, f32 demotes after every
      op, float→int saturates AT THE TARGET WIDTH through exact f64
      boundary compares (59, plus float-typed enum payload binders
      at 60); the exit code masks at 0xff like boot's kernel (61
      with 022/058); eprintf on fd 2, if-expressions (the match-arm
      block tail parses one naturally — 076's burn recursion), and
      honest bool holes (63); aggregate let completes — slice
      literals [a,b,c], string-valued ifs, and the make builtin
      demanding 'make([' so a user fn may own the name (64);
      slice-returning fns ride the pair ABI. The forty-fourth
      through fifty-first cuts (2026-09-26, one session) close most
      of that remainder: the exact-decimal float printer rides a
      rho-source cluster the emitter compiles through itself
      (FLOATSRC beside the program when a float hole prints — the
      lexer's exponent-after-fraction bug fell out, and a MUT string
      let materializes its pair so a never-entered growth loop keeps
      its initializer); bool and string consts join every env and
      every bool hole prints its word (the ! desugar's eq-0 included);
      string payloads ride enum boxes as pairs (Err's text finally
      travels; struct-payload patterns bind after the colon or by
      their own bare name); string statics carry their pair in two
      wasm globals; a let over a string/slice name aliases its
      binding; generic fns monomorphize per type binding (bind, clone,
      substitute, drain); overloads resolve exact-match-unique under
      signature keys and values print through their type's to_str
      (printf and format holes alike); variadic rest params
      materialize at the call (a lone spread rides whole) and
      []string slices store and read their pairs at a 16-byte stride.
      81 of 108 behavioral, floor 81. The fifty-second through
      fifty-fourth cuts (2026-09-26, same session): a format hole
      holding another string build rides its built pair (a nested
      format printed its ADDRESS — n10), and closures land whole —
      the fn-literal parses in expression position, its value is
      (table idx, capture box) with pointer captures typed, calls go
      through call_indirect over a funcref table, plain fns ride
      trampolines as values, and fn-typed params/returns ride the
      pair ABI as a third kind (eat_type consuming 'fn' as a bare
      name was the silent killer underneath every fn-typed
      annotation). 84 of 108 behavioral, floor 84. The fifty-fifth
      and fifty-sixth cuts (2026-09-26): the parser's silent
      truncations die — bare unary `*` parses (only `(*e)` did; the
      fallback ate the star and the leaked field block's `}` closed
      the enclosing fn early), bare `{ stmts }` blocks parse (boot's
      T_LBRACE form), the match statement eats its trailing `;`
      (CORRECTED 2026-10-03, the T3.18 residue closure: that was the
      mirror alone — boot's statement match shares the expression
      form's parser and never ate it; the mirror now refuses it with
      boot's exact wording), and
      every parse fallback counts PCur.perrs which main refuses
      before emitting — a clean exit-1 refusal instead of a hollow
      program; the match-arm tail probe restores the count on rewind
      (a statement head probed as an expression is not an error).
      Then by-value structs land whole: a struct-typed field inlines
      the nested struct's slots; `h.p.a` chains expose the
      sub-struct's ADDRESS (frame-rooted chains flatten to one
      local.get); `new T { f: *new U{...} }` copies the slots in;
      a by-value param rides one address lane whose callee prologue
      copies into a fresh frame; a struct-typed return rides its
      box address; `let q: Pair = <any struct expr>` copies through
      struct_addr_of (frame-resident chains materialize a box — a
      wasm frame has no address); `make([]T, n)` sizes struct
      elements by their slot block and string elements at their
      16-byte pairs (a pre-existing under-allocation read past the
      block); struct-element slices index/store the whole block;
      string-element compares ride push_string (a pre-existing bug
      compared the pair's ADDRESS against the literal's — every
      element of a fresh slice read non-empty); the slice-element
      table becomes a real stack (a pre-existing one-slot clobber:
      the second slice binding in a scope erased the first's element
      type — 102's match then compared a box pointer against tag 1);
      and $w_alloc grows memory past the 64KB first page (a
      pre-existing exhaustion crash — 400 allocation rounds faulted
      at 0xffffff68). 101, 102, and 093 land byte-exact. 87 of 108
      behavioral, floor 87; five byval selfhost legs pin the wave.
      The fifty-seventh cut (2026-09-26): generic structs, enums,
      and methods. Declarations parse their type parameters; eat_type
      and `new T[a, b]` normalize explicit instantiations into the
      type text (`Pair[i32,i64]`); st_index/en_index instantiate
      lazily under the full text (fields and payload types substitute,
      variant slots recompute, the tables double through parse's
      registration constructors — parse owns every `new`). tsub_type
      substitutes inside prefixes and bracket args alike (`*T` ->
      `*Pt` — the silent killer: it only matched whole texts, so
      generic params never became real types); T binds directly or
      structurally (`*Pair[A,B]` unified with `*Pair[i32,i64]`); a
      generic fn's clone rewrites its ABI flags when a parameter
      binds to string/[]T; methods clone per receiver binding (the
      receiver's written type takes the full instantiation so field
      reads key on true layouts), and a generic enum's method return
      substitutes through the receiver. Along the way: mangle_type
      sanitizes bracketed args (a `Pair[*Pair[...]]` id once carried
      raw `[`/`,` into the wasm name); expr_ptr_type resolves slice
      elements (pointer elements strip their star) and enum-receiver
      method returns; build_string_pair grew a string-FIELD branch
      (a pre-existing gap — `outer.b` printed its pool address) and
      the primitive to_str yields when the receiver's own type
      carries one; bool-typed params print their words; norm_enum no
      longer collapses a user `Opt[i32]` into Option; call_fn_rtype
      guards the empty fn table (len() on a fn-less program
      panicked the compiler). 018, 080, 081, and 083 land byte-exact:
      91 of 108 behavioral, floor 91; gens1-3 selfhost legs pin the
      wave. The fifty-eighth cut (2026-09-26): rc and weak land
      whole. Every box carries its rc in the slot BEFORE the payload
      pointer (all existing offsets untouched); $w_alloc stamps rc=1
      and grows memory past the first page — with a floor that rises
      to the old end on every grow (the first version marched the
      new top down through lived-in pages and corrupted the shared
      None box at 65504; 102 died at round ~60). weak[T] rides the
      pointer lane uncounted (weak.from is the identity, equality is
      identity); get() probes the header and its fresh box retains
      the payload INSIDE the live branch (a dead probe must never
      resurrect). The counting law: rebinds retain the new value
      first (self-assign stays alive), then release the old through
      the type's thunk — a lazily-registered per-type walker
      (struct fields, option payloads by tag, enum variants, slice
      elements; strings and scalars release nothing — their deaths
      are unobservable); construction retains shared inits into
      fields, enum payloads, and slice elements; a scope's owned
      lets release at scope exit through the defer stack's new
      release pool (returns unwind it too, SKIPPING the returned
      binding — the value moves out with its count); match binders
      borrow (never counted); a fresh scrutinee box dies with its
      match. ?X params bind the option lane (a pre-existing hole:
      they bound as pointers, so matches over them compared box
      addresses against tags); Option patterns carry string payloads
      through optpt; panic(msg) is the fatal builtin (exit 101);
      match expressions whose arms spell bool words print them.
      n02, n12, n13, n14, 056, 095, and t03 land byte-exact — and
      102's grow-floor fix keeps it. 98 of 108 behavioral, floor 98;
      weak1-5 selfhost legs pin the wave. The fifty-ninth cut
      (2026-09-26): traits, impl blocks, and dyn PARSE. Trait
      declarations record their methods in order (the vtable
      ordinals) and each declared return type (the dispatch's calling
      shape); impl blocks register their fns as the type's methods
      through an impl_type carrier on the cursor; `dyn Trait` rides
      as a type text. The dyn EMIT (fat {vtable, obj} pairs, dispatch
      through the fn table) is built but DISABLED at its three hooks
      — the coercion site crashes indirect calls (084); the static
      accident handles homogeneous dyn, so the hooks return when the
      crash is fixed. A slice-field let records its element text (a
      match over old[j] lost its binder's pointer lane), the release
      walker pushes slice fields as pairs, and an Option scrutinee's
      payload target resolves through a binding, an element, or a
      struct-field slice. 084 and 085 land byte-exact (name-
      satisfaction and bounds are static dispatch): 100 of 108
      behavioral, floor 100. A follow-up fixed the trait signature
      capture (eat_arrow swallows the arrow's type itself — the
      rtypes rode as i64 defaults; eat_arrow_typed's return is the
      type) and broadened the parked hole branch to any string-
      returning dyn method — with the hooks re-disabled, 084/085/t03
      all hold and the trait table now carries true rtypes for the
      re-landing. The sixtieth cut (2026-09-26) re-lands the dyn emit
      whole: the dispatched pair reserves its slots BEFORE the args
      (callers key on the reservation), the vtable gains a release
      slot at nmethods (a dyn's death dispatches through it — the
      mirror of boot's header drop; rebinds, element stores, field
      walkers, and scope exits all release there, with a null-obj
      guard for zeroed elements), dyn slots ride the honest lane
      accounting, dyn-typed fields and slice elements lay out as
      16-byte fat pairs (make sizes, sub-slice strides, and stores
      follow), a dyn method call renders through its vtable whether
      the receiver is a binding, a field, or an element (string
      results ride the pair; scalars print through $w_itoa — the
      shared-scratch $w_i64 clobbered earlier holes' text), and the
      coercion sites cover let/field-init/assignment/element-store.
      Along the way three generic holes closed: a clone's substituted
      slice param never flagged its pair ABI ($u_len fell out of
      089), expr_type typed slice bindings (T bound from []*T), and
      expr_ptr_type read the DECLARED return of a generic call — the
      nested dup(dup(2)) bound Pair[T,T] instead of the
      instantiation. Plus 105's pointer-base slice-field store now
      writes BOTH pair words (it dropped the len). 030, 084, 086,
      087, 088, 089, and 105 land byte-exact: 106 of 108 behavioral,
      floor 106. The sixty-first cut (2026-09-26) lands module
      loading and the corpus differential CLOSES at 108/108: the tree
      rides the MODS build parameter ("@MOD@ <path>\n<text>" blocks
      — boot reads the same tree from disk), parse grows
      `use a.b as x;` / `pub fn` / `pub use` with the export law (a
      facade sells its pub fns; `pub use X;` flattens the bound
      module — binding or path — and `pub use X.item as Y;` renames
      one), modules register under dotted canonical names with their
      internal bare calls qualified, and every `binding.item(...)`
      call in main and the fn bodies rewrites into the plain call of
      the exported fn. 032 and t09 land byte-exact. The mut law and
      usize lane remain (T3.6's mirror side)*
- [x] **T2.x** Optimizer in the mirror: constant folding, dead-code
      elimination, globals tree-shaking, tail-call→loop — with the
      language suites (opt/eq/params/modsys/strops/multiline) rebuilt
      for the new language. wasm emit only — **no native backends in
      0.1.0.** The wave lands as one consolidation pass: after §17–§19
      and T3.5–T3.8 are in, boot and the mirror get a refinement sweep
      (structure, naming, dedup of the growths' accumulated patterns)
      before the suites pin them — features first, polish second,
      never interleaved.
      LANDED 2026-10-04, the optimizer wave on the frozen toolchain:
      global tree-shaking (per-fn reachability from `$_start`;
      collections −15%, closures −33%, hello −51%), decoded-content
      string-pool sharing, comptime-fold DCE, live-range slot
      recycling, operand-stack returns, the dead-fn sweep (−177
      lines), literal-retain elision (general escape analysis
      descoped — closure captures and weak's death-observable law
      outlive a tooling wave). The wave flushed two self-chain
      codegen bugs, both pinned (n26 payload-literal pins, n27
      block-scope pops + fkind clears); wasm-opt stays on the site
      pipeline (the chain product is 2.84× the wasm-opt product —
      the retirement bar is unmet).

## Phase 3 — gates and the trust root

- [x] **T3.0** GitHub CI: one workflow on push + PR — clang builds
      boot, `make test` runs as the leg (every script already
      time-capped); wabt and wasmtime pinned by version, never
      floating. When T3.1's gate.sh lands it becomes the leg list.
      The workflow is dormant until master pushes resume (push timing
      stays the owner's call). The native ring's ephemeral-runner CI
      (T7.x) rides this same workflow when it exists.
- [x] **T3.1** gate.sh rebuilt: boot selftest; corpus differential
      (boot-built vs self-hosted-built, behavioral); diagnostic parity;
      the self chain (mirror → child → grandchild, graded behaviorally).
      tools/gate.sh exists with every leg time-capped; legs 1-3, 5-6
      (selftest, scripts, the 108/108 corpus differential, the seed
      canary, diagnostic parity) pass. Leg 4 — the self chain — is
      blocked on the mirror's first-ever run shaking out scale bugs,
      five of them fixed in one session (2026-09-26): memory sized to
      the data segments, the heap starting past the data top, the
      immortal guard reading data_top instead of the moving heap, the
      fmt scratch keeping its honest 4032-byte cap (a page-wide
      scratch corrupted large outputs), rho_panic writing one iov per
      piece (wasmtime 40 drops trailing iovs), module consts
      exported (the compiler source is full of lex.TK_* field reads),
      and the parser's struct-literal/declaration field tables grow
      on demand (a fixed cap was an out-of-bounds write the day a
      struct rode eleven fields). The index-first field store
      (base[i].f = e) parses and emits, and a loose file module owns
      its parent directory. RESOLVED 2026-09-26 (the consolidation
      night): the parse crash was never a grammar — match_chain_m's
      NINE-parameter header indexed past parse_params2's fixed cap of
      eight; the tables scrambled and the parser collapsed behind it
      (the fn-span stubber died unneeded). count_params pre-counts the
      list (bracketed type commas never count) and every param table
      sizes from it; the module merge now registers structs and enums
      (mods_struct_add/mods_enum_add — module-local `new` and match
      were unknown to the checker). Two corpus legs pin both (106 the
      ten-param signature, 107 the in-package struct and enum; floor
      108 → 110). The mirror now FORMATS its entire own source (320
      KB through FMT=1, rc 0) and its emission runs to the 4 GiB
      ceiling: the never-reusing concat allocator's quadratic —
      `s = s + piece` at MB scale (a 1.2 MB build dies at ~30 K
      concats through boot's own kernel too; measured).
      THE APPEND LAW — the single-tail form LANDED green with the
      whole gate (the lang suite pins 30 K appends linear, 1.2 MB):
      `$rho_app(a, al, b, bl)` extends in place when a ≥ data_top,
      rc(a)==1 (the counting law is complete for strings —
      assignment/argument/field retains all emit), and al+bl ≤ sz
      (capacity rides the block header; cat2 results carry half-again
      headroom so a cat-fed accumulator is append-ready). The
      flattened multi-tail chain corrupts exactly one piece when a
      tail rides the to_str/format fusion (the fb two-phase assembly
      interleaves with sequential apps — a `(if len>0` elision
      swallowed adjacent literal pieces; gens3's child lost one
      `(local.get N)` line) — longer chains stay on cat2. The
      MIRROR still quakes: mechanically splitting its 283 chains
      into single-tail statements breaks at emit.rho's chain 91
      (push_string's i64.const push) with a check-stage
      `index out of bounds` INSIDE the mirror — SOLVED: the mirror's
      MATCH ARM bodies rode a fixed cap of 16 (the fourth holdout of
      the fixed-cap class — fn bodies 64, bare blocks 64, closure
      bodies 16, arms 16; stmt_grow doubles them all on demand).
      With the 283 chains split (single-tail statements, indentation
      preserved) and the append law live, THE MIRROR COMPILES
      ITSELF: rc 0, 148,607 lines of child WAT in 0.43 s, FMT self
      338 KB clean. The self-CHAIN's open gap narrows: the child
      called \$u_len 292 times — len()'s fall-through was a call to
      a fn never defined (a latent gap no corpus program rides);
      kind-2 slice bindings and slice-typed struct fields now read
      their pair slots directly (224 close; pair-returning calls and
      slice-of-slice elements join them). 68 remain, censused:
      field reads over generic-instance bases (len(sd.tparams) —
      field_base_type/st_index miss the instantiated name), match
      binders (bound kind-less over slice payloads), and the tag-6
      anonymous form — the last emission slice before gate.sh leg 4
      (child/grandchild byte-identity + the probe) is the 0.1.0
      heart beating.
      THE CENSUS CLOSED (2026-09-27): $u_len is GONE (68 → 0) and
      the fix surfaced what the undefined calls had masked — wabt
      skips validation when resolution fails, and behind it sat 217
      VALIDATION-level miscompiles in the child WAT, now 125 and
      falling. Fixed families (each pinned by boot-vs-mirror repros
      s1–s6): element strides (string/[]T/dyn elements ride 16-byte
      pairs — the VAR-base, FIELD-base, and make/literal stores all
      read stride 8 against stride-16 writes); len() over
      string-element slice elements; return/args of slice VIEWS
      (xs[a..b] fell through emit_expr into the BIN template — a
      view's op=3 IS the div opcode, so every view arg rendered as
      a bogus division; build_string_pair now widens slice-typed
      bases); let over a slice-returning call (the string build
      claimed the kind-1 lane — now the []X return parks its pair
      and records its element text); an Option-scrutinee's payload
      through a ?*T FIELD (st.val binders ride the pointer lane);
      expr_ptr_type through indexed fields (p.tbl[i]); array
      literals with pair elements (16-stride pair stores, elemtext
      from the annotation); oob checks emitted BEFORE the index they
      check.
      THE SECOND CENSUS WAVE (2026-09-27, 125 → 57): four more
      families, each pinned by a run-selfhost leg — (1) `return xs`
      over a plain slice/string binding fell into the single-value
      return template (build_string_pair's EX_VAR branch now takes
      kind-2 beside kind-1; pairret); (2) len(f(...)) — the len
      template called emit_expr for the arg, but emit_expr's sret
      branch collapses a pair-returning call onto its address lane,
      stranding the template's second pop — the pair now comes from
      build_string_pair directly (lenpaircall); (3) a slice rebind
      (body = grow(body)) had NO ST_SET branch — the scalar lane
      pushed the address alone and stranded it on the branch stack
      while the binding's slots still held the stale initial value
      (18-vs-20 behavioral divergence) — kind-2 rebinds now write
      both pair slots in place (slicerebind); (4) `f(make([]T, n))` —
      make-as-argument fell through emit_expr into the BIN template
      and rendered as a lone (i64.add) (make's op is 0) — emit_arg
      grows an EX_MAKE branch that rides the fresh pair into the
      param slots (makearg). A fifth fix rides along: a CALL
      scrutinee's Option-payload binder lanes (optpt_of_scrutinee
      resolves the declared return — match fold_string(...) now
      binds its string payload as a pair).  The corpus differential
      stays 110/110 through the wave. PARITY NOTE (2026-09-27):
      boot's checker refuses a slice SPREAD at a variadic call
      (w("head", xs...)) while the mirror compiles and runs it —
      the mirror is the more permissive side; whichever way the law
      rules, the differential never rides it.
      THE THIRD CENSUS WAVE (2026-09-27, 57 → 15): three silent
      miscompiles and the big one, each pinned by a run-selfhost leg
      — (1) an Option FIELD scrutinee compared the box POINTER
      against the tag constants (a fresh Some never matched 1 and the
      match fell through every arm): expr_enum_type resolves a struct
      field's declared type (?T is the Option box, a named field its
      enum) (optfieldmatch); (2) indexing a struct-typed slice FIELD
      read one 8-byte slot at stride 8 — the element's whole slot
      block is the stride and the ADDRESS is the value, both frame
      and heap paths (the var-base branch's law, missing on the field
      base) (fslacelem); (3) expr_ptr_type through an indexed field
      only accepted *T elements — a value-struct element left the
      let unbound to a pointer and every field read off it died as
      const 0 (rides with fslacelem); (4) THE BIG ONE: a slice-typed
      field as an argument pushed its address half alone —
      push_string's field gate and build_string_pair's field branch
      now take slice fields beside strings (the pair layout is
      identical) (slicefieldarg) — this one family was ~40 of the
      57. THE FOURTH WAVE (same day, all silent miscompiles, pinned
      by the strstore leg): (1) a string-element ISTORE evaluated the
      RHS pair build AFTER parking the LHS index in $t — the build's
      own element reads clobbered it and bs[2] = bs[1] wrote element
      1 twice; the LHS index now parks in its own slot; (2) the
      indexed FIELD store (base.f[i] = e) hardcoded one 8-byte slot
      at stride 8 — pair elements now store both halves at 16, a
      struct element its whole slot block. Still open in the child
      (15): six orphan (i64.add)s in emit_arg's self-compile, five
      branch-tail surpluses (dyn_hole_dispatch's arm tails), two
      pair-return singles, a pair of one-offs.
      THE FIFTH WAVE (same day, silent, pinned by the fieldmakeelem
      leg): a make riding a NEW FIELD INIT allocated its COUNT in
      bytes, not count-by-element-width — a []string field of 4 held
      a 4-byte block and every element store wrote the neighboring
      heap (element [1] survived by luck, [2] clobbered [1]'s
      neighborhood). The field-init make now sizes by the element
      (16-byte pairs, struct slot blocks) like every other make
      site. The corpus differential stays 110/110.
- [x] **T3.2 Pure-source trust root**: every gate run rebuilds the seed
      from boot's C source on the spot. The pinned `seed.wasm` stays in
      the repo **as a canary**: rebuild, compare byte-for-byte (D1 makes
      this exact), inequality = determinism alarm. **Landed 2026-10-01**:
      `build/seed.wasm` pinned (gitignore exception — the one build/
      artifact in the repo), gate leg 5 enforces the byte compare and
      fails the gate when the pin is missing; re-pins ride the commit
      that changes the compiler.
- [x] **T3.3** Differential fuzzing rebuilt for one implementation:
      the framework lives as `tools/fuzz/gen.mjs` (seeded LCG, fully
      reproducible by seed — `--emit N` regenerates any program),
      running the SAME random program through boot and the
      self-hosted chain (a fresh per-program bake, run-corpus-diff's
      construction) and comparing stdout + exit code; gate leg 2d
      runs seeds 1..150 every gate. First campaign 2026-10-01: 150/150
      green after four finds, each pinned as a corpus case (n22-n25
      name their seeds). The golden corpus replay rides gate leg 3.
      The opt-on vs opt-off arm is vacuous at 0.1.0 — there is no
      optimizer to differ (§13 is post-freeze backlog); it joins when
      the optimizer does.
- [x] **T3.5** The test protocol (§17) — **lands before T3.4, so the
      suites are written directly on the real verb and no second runner
      ever exists.** boot grows the `test` block and rebuilds the
      `rho test` verb from its script-forwarding stub into the real
      runner: lex/parse/check/emit/fmt for the block form (fmt
      round-trips it like any decl); paths or directories — a directory
      picks up `*_test.rho` files and case directories with a `main.rho`
      entry, name sorted; every test compiles as its own program
      instance (per-test isolation under the time cap); filter; honest
      zero-tests exit 1. Golden headers: `// out:` / `// exit:` /
      `// set:` (§17), plus the three the suites need — `// expect:`
      (diagnostic: check must fail with every named substring present),
      `// err:` (stderr substrings — pins the panic catalog's messages),
      `// pending:` (expected-fail ledger for law ratified but not yet
      implemented; a pending case that passes prints a PROMOTE notice,
      promotion = the marker comes off). Fixtures are the suites' first
      directories; the suite leg wires into make test. The self-hosted
      compiler mirrors the surface when its CLI lands (a T2 dogfood
      leg: rho's test suite running rho). Landed 2026-09-26: the
      block form through lex/parse/check/emit/fmt, the in-binary
      runner (collection, name sort, filter, per-test isolation under
      a 10 s hard cap, honest zero-match exit 1), the six headers with
      PROMOTE counted as failure until the marker comes off, and the
      suites leg wired into make test.
- [x] **T3.4** Suites: lang/modsys/opt/eq/params/multiline/strops/diag
      rebuilt for the new language, incl. the fixes the design mandates
      (aggregate let-position values are legal; `as` truncates constants
      like variables; bitwise compound assignments verified end-to-end).
      This is spec.md §11's conformance map made executable: the suite
      lives in `tests/` under the map's directory names, every case is
      a `*_test.rho` file in §17 form (`// out:` / `// exit:` /
      `// set:`; a diagnostic carries `// expect:` substrings and must
      fail check), and every file names its law in a
      `// spec: <doc> §<n>` anchor — the §11 rule→test table generates
      from the anchors, and a rule whose test does not exist is a rule
      not implemented. Multi-file cases (packages, facades) are
      directories with a `main.rho` entry carrying the same headers.
      FIELD-INIT EXACTNESS (2026-09-27, found while building
      self-chain repros): a field initializer never compared its
      value's type against the field's — `val: new S{...}` into a
      `?*S` field parked the raw pointer and a match over the field
      read the box header as a tag (neither arm ran; let/param/
      element stores all refused the same shape — the field init was
      the one silent hole). The face is now exact like a let's
      annotation, with the value-struct boxed copy law (T
      f: *new U{...}) the one legal widening; two check fixtures pin
      the refusal and the explicit-Some behavior.
      VALUE-STRUCT FACE (closed 2026-09-26 by probe + fix): the
      grammar-audit round found nested value-struct construction
      failing to emit — the FIELDINIT staging stuffed the
      initializer's box pointer into the field's flat slot run. The
      landed model: a boxed initializer for an inline value-struct
      field stages the pointer and the parent owns field-wise copies
      of the payload; value-struct expressions ride flat field runs
      (construction, reads, lets, value params all verified), and the
      §10 element-wise struct `==` compares each slot in its own face
      (floats by IEEE eq). `new V{}` itself stays `*V` — pointer
      identity == for freshly boxed structs is eq_pointer_identity's
      pinned law. Also closed in the same audit: `make([][]T, n)`
      (the type-arg parse consumed the outer `[]`; the checker then
      mistook the slice element for an already-sliced type), and the
      test-block leg of `rho test` — a block name with a space
      produced an illegal wat identifier (`$test:always true`) so
      wat2wasm failed and every block test was misjudged as panicked;
      fn_wat_name now folds non-idchars to `_`. Four new lang
      fixtures pin all of it; suites 98/0/0.
      Two tiers: green files gate; a case for law already ratified
      but not yet implemented (§18 mut view, §19 match ergonomics,
      T3.7 item imports, the T3.6 usize lane) carries
      `// pending: T3.x` and runs as an expected-fail ledger,
      promoting per case when its task lands. The suite runs on
      T3.5's verb — it is the verb's fixtures and acceptance; no
      interim runner exists. ~~The existing tests/check negatives
      stay put — no migration churn.~~ MIGRATED (2026-09-27): the
      verb is now the main test framework. tests/emit (30 behavioral
      fixtures) moved to suites/emit as *_test.rho — they now RUN
      under the verb with their `// out:` pins (stronger than the old
      stdout-only leg: exit codes are judged too; pos_labels pins
      `// exit: 1`); tests/check (24 diagnostic + 8 positive) moved
      to suites/check — expect: files judge whole, positives gained
      `// out:` pins and actually execute; run-set-tests' six --set
      faces became suites/set (one variant per override, the range
      refusal rides `// expect:` after the refusal learned to speak
      diag). The migration caught and fixed a real verb bug:
      g_set_refused was a process latch — one refused case poisoned
      every later compile; it now resets per case (the isolation
      law). run-check/emit/set-tests.sh retired; make test and
      gate.sh run the suites (gate leg 2b, 900 s cap) plus only the
      cross-compiler shell legs (fmt roundtrip, fmt-self, selfhost,
      differentials, corpus replay, robustness). The skeleton is
      seeded (lang/modsys/eq/params/strops/multiline/opt/diag, green
      tier + the §18/§19/T3.7 pending ledger); the full §11 map keeps
      growing per area until the map is complete.
- [x] **T3.6** The mut view law (§18) + the usize lane. Boot surface
      map: parse grows the `mut` argument prefix in call argument
      lists (params already carry the flag); check_fn_body keeps the
      parameter's declared mut instead of forcing false; dotted,
      indexed, and compound stores through handle bindings gate on
      the root binding's mut; mut-receiver method calls gate the same
      way; argument markers pair with the parameter in both
      directions and require the argument's root binding to be mut;
      sig_same and impl/trait matching join receiver mut-ness (two
      candidates differing only in `mut self` are ambiguous); fmt
      renders the argument marker (it already prints param/let/static
      mut). Emitter and kernel: zero — permission never layout. The
      corpus adapts mechanically (`let` → `let mut` on
      through-written bindings) in the same commit, plus the leg
      pinning that a value receiver cannot call a pointer-receiver
      method. The usize lane comes home to the address width (u32 on
      wasm32; syntax.md §3): len, indexing, alloc counts included.
      The self-host mirrors both when it grows the matching surface.
      Acceptance legs (each gate above is a leg, same commit):
      rejections — store through non-mut binding, mut-method on
      non-mut binding, marker missing where required, marker where
      not allowed, marker on non-mut root binding, `mut` on a value
      parameter, value receiver on pointer method; behavior — mut
      binding stores, `let mut p = p;` rebind idiom, handle copied
      out of read-only is writable by its own binding, compound
      assign through a mut view; match — impl/trait receiver-mut
      mismatch, dual `mut self` overload ambiguity; fmt — the
      argument marker round-trips.
      Landed: `mut` argument markers ride NT_POSARG op=2 (make's type
      arg keeps op=1); ParamDef.is_mut flows from parse through trait
      sigs, closures, and generic instances; check_fn_body keeps the
      declared mut; field/index/compound stores gate on the root
      binding (the pointer free-pass is gone — a pointer is a handle);
      mut receivers gate the root binding; markers pair both
      directions, with a mut-blind retry that diagnoses the exact
      argument even under a quiet probe (err_at respects the probe,
      so the diagnosis bypasses it — a real error, not selection);
      sig_same joins mut-ness (self/mut self twins are distinct and
      ambiguous together); trait satisfaction joins receiver mut;
      value params refuse `mut`. Emitter and kernel: zero layout —
      only the usize lane (i64 → i32, the wasm32 address width) with
      its consumers unwrapped, plus a defined `allocation too large`
      panic when make's byte size exceeds 1 GiB (was a silent wrap).
      fmt renders the marker; suites 71/0/0 (the three pending_mut
      legs promoted); corpus + libs + prelude adapted mechanically
      (let mut, mut view params, markers, the `let mut p = p;` rebind
      idiom for match binders). Mirror note: the self-host still
      parses neither markers nor `mut self` (092/098 :refuse) and
      computes usize at 64 bits (041/046/048 :diff, 005 :w2w) — those
      five legs close when the mirror grows the matching surface; the
      differential floor is re-pinned 91 → 85 with justification in
      run-corpus-diff.sh.
- [x] **T3.7** Module item imports (module-system.md §2/§4/§6,
      syntax.md §4.4): the final use-segment binds a public item
      unqualified (`use lex.a as b;`), the brace form expands one
      plain use per item, module-vs-item and name collisions are
      errors naming both, and item imports stop at package facades.
      Landed: parse (`use a.{b, c as d};` — parse_use_segs leaves the
      cursor on `{`), a post-BFS item pass (bind_item_imports — every
      owner prepared, so module-first-then-item sees both candidates;
      the across-kinds ambiguity names module path and owner), copies
      of pub Syms under the alias (Sym.imported gives the collision
      law its voice: two-imports vs import-vs-local), the package
      facade stop for item forms, and the §4 interior closure (a file
      inside a package directory imports only from siblings — the
      facade crosses freely). Seven modsys fixtures; suites 54/0/3;
      corpus differential floor 91 untouched. The self-host mirrors
      (floor per T3.4). Follow-ups landed with the law fixtures: the
      bare `pub use inner.tool;` re-export (the owner resolves even
      without a sibling `use inner;`), the grammar's trailing comma
      in the brace form, and the empty-brace refusal. Ledgered
      divergence: `pub use inner;` (the module re-export form)
      flattens inner's public items instead of binding the submodule
      under the facade — the consumer's `pkg.inner` needs nested
      module-qualified access, machinery the checker does not have
      yet; until then the §6 module form behaves star-like.
- [x] **T3.8** The compiler IR and the in-tree binary path — RULING
      2026-09-27: adopt the archive's proven hub shape, strict SSA
      over scalar virtual registers with structured control flow
      (reducible CFGs from if/while/loop; phis exactly at value
      merges; frame slots for everything else), restored onto the new
      checker. Edges, each with its own byte acceptance:
      (1) check → lower → IR — new code, ported shapes;
      (2) IR → WAT serializer — byte-identical to today's emitted WAT
      over the whole corpus, every suite, and the self-chain (the
      one-time wholesale re-pin; T3.10's mirror work is untouched —
      the IR does not go near the checker);
      (3) IR → wasm binary serializer — byte-differential against
      wat2wasm over that same WAT until green, then wat2wasm retires
      from every default path and the T3.2 seed canary re-pins from
      boot's own bytes (its first meaningful pin);
      (4) WAT → IR decoder — the wat→wasm→wat fixpoint leg, the
      debug/interchange contract, and the future std.wasm cross-pin;
      (5) optimizer passes ride the pinned hub afterwards, in design
      §13's order (rc-pair elimination / escape analysis first-class;
      linear-scan register allocation waits for the Phase-7 native
      backends — the wasm operand stack needs none). Boot stays
      wasm-only: native emission belongs to the self-hosted side
      (the Phase-7 std-library components, built on std.wasm) — the
      IR hub's serializer edges are WAT and wasm, nothing else. The
      mirror keeps its original sequencing: `std.wasm`, the
      encoder/decoder package written in rho, still lands with the
      differential closed, byte-pinned against boot's assembler.
      Two riders land before the byte-pin freezes (ruled
      2026-09-27): the per-function `$tco` loop wrapper is emitted
      only for functions with a direct self tail call — today every
      body gets one, and the wholesale re-pin would freeze the wart
      into the goldens; and the T3.2 re-pin ships with a reseed tool
      (the archive's tools/reseed.sh shape) so re-pinning the canary
      stays a repeatable operation, not a one-off.
- [x] **T3.10** The bug-fix-wave mirror re-adaptation (opened
      2026-09-26): boot grew six real fixes (value-struct field
      stores, float compound assignment, the narrow-width shift mask,
      the comptime &&/|| bool fold, static-from-const initializers,
      ordered float compares) and the mirror binary — compiled BY
      boot — moved with them: its parser/checker state machines were
      calibrated against the broken faces (feature tables kept their
      initial values because field stores silently vanished; folded
      conditions took the wrong branch). The corpus differential sits
      at floor 84 with 24 legs open (19 :refuse — the mirror lacks
      traits/dyn/package/pub-use-forms/weak/rc and friends; 4 :diff on
      041/045/046/048 — the new mask law; 1 :w2w — 005 still emits on
      the old 64-bit usize lane; zero :build — boot compiles the
      mirror cleanly). The mirror source needs the same error-driven
      adaptation the mut law got; each leg that closes lifts the
      floor. The boot-side hygiene ledger from the coverage agents
      closed same-day (the hygiene wave): `pub use sub;` re-export
      landed with T3.11 (B-7 done); three dead diagnostics pruned
      (resolve_type's unreachable "invalid type syntax", the never-
      called const_resolve_all and its report path, the format-values
      non-POSARG arm); "tuple variants bind positionally" was dead as
      written and now fires where its case actually lands — a braced
      pattern on a tuple payload refuses "named patterns need a
      struct-form variant"; parse.c's "cannot open file" is ALIVE (the
      entry-file message — agent misjudged); tok_spell's bare/quoted
      split is a convention (categories bare, literals quoted — now
      documented at the table, zero drift found); and the quadratic
      check time was not check at all — arena_alloc's grow path
      created a fresh block AND walked the whole chain per overflowing
      allocation once the 1 MiB head filled; continuing from the tail
      made 16k fns go 15 s → 0.53 s, every compile faster. The sweep
      also uncovered and fixed a real silent-miscompile family: const
      initializers that read another module's const (qualified b.K,
      item import, facade) folded too early and silently read 0 —
      consts_converge now re-folds post-BFS and the comptime law is
      hard (cyclic / non-comptime / annotation-mismatch consts error).
      Still open from the old ledger: the float-literal-beyond-range
      → inf spec ruling — ruled in T3.12 (out of range = compile
      error, never silent inf). The strictness question parked here —
      positional binding of a struct-form variant (`V(x)` on
      `V { x }`) — ruled in T3.12 as well: the pattern form must
      match the payload form, both directions refuse.
- [x] **T3.11** Grammar-campaign leftovers (opened and closed
      2026-09-26): the 68-fixture grammar suite (tests/suites/grammar)
      closed one crash (`Option.None?` segfault — ? now infers the
      payload from the enclosing return) and eight gaps (struct
      patterns §7, immediate closure calls `make_adder(5)(3)`, float
      literal patterns, `s += "cd"` on strings, `mut` on string/dyn
      params, comma-less block match arms, variant-payload trailing
      commas, subset variant binders by name); the same wave closed
      the rest: `pub use sub;` module re-export resolves through the
      facade (qualify_module walks pub module-use chains); the §6.8
      capture law lands REFINED — a mut local of VALUE type may not
      be captured (the copy would diverge on rebind), a mut HANDLE
      captures fine (the copy is the shared view; the heap object
      holds the state — the escape hatch §6.8's rationale names;
      corpus 078 pins it. Spec pass pending: §6.8's wording should
      narrow to "a mut local of value type"); parameter-list trailing
      commas refuse (§4.1 has no ','? there); the fn-body tail
      expression is the body's value — typed against the return at
      check, turned into the return at emit (`fn f() -> i32 { 3 }`
      returns 3). make test green end to end: 436 pass, 0 fail,
      0 pending — no expected-fail ledger left in the suites.
- [x] **T3.12** The operator traits — Eq, Ord, Hash (opened
      2026-09-26; design §11 amended, docs landed in the same wave).
      ENTRY POINTS SURVEYED (2026-09-27, pre-work): boot's keyword
      table is boot/lex.c k_keywords + the TokKind enum in rho.h
      (add K_SELF AFTER K_STRING — the K_I8..K_STRING builtin range
      check in parse_type_inner must stay intact); parse_type_inner's
      default arm (boot/parse.c ~line 1280) takes T_IDENT to a named
      type — a `case K_SELF:` there yields the "Self" name; the
      impl-registration pass is boot/check.c:1415 (impl members
      become methods via f->name2 = target name) — the Self
      substitution belongs where an impl member's signature types
      resolve, aliasing "Self" to the impl's target (and to the
      trait's placeholder inside trait sigs); trait decl parsing is
      boot/parse.c:1477. The full law is design §11 + type-system.md §15; the two parked
      rulings also ruled here: a float literal that rounds to ±inf or
      (from a nonzero literal) to zero is out of range — a compile
      error, never a silent inf (type-system.md §1's fit law, now
      enforced for floats); a variant pattern's binder form must match
      the declared payload form — braced on a tuple payload and
      positional on a struct payload both refuse (syntax.md §7).
      Construction order: parse `Self` (keyword, type positions in
      trait/impl contexts) → check resolves the six operators through
      trait lookup with the choice pinned on the node (op=5 style) →
      emit direct calls → bounds verified per instantiation → corpus +
      fixture regression, differential floor untouched. Acceptance:
      default slotwise `==` unchanged everywhere; `impl Eq` overrides;
      `<` on user types refuses without `impl Ord`; the never-list
      stays locked; `impl Hash` replaces the FNV default; `[T: Eq]`
      dispatches statically; `dyn Eq` `.eq` dispatches virtually
      (ordinary §9 dyn); cycle-through-user-eq ends in the documented
      stack-overflow panic.
- [x] **T3.9** Match arm ergonomics (§19): variant resolution by
      scrutinee (bare `Some`/`None`/`Ok`/`Err` and user-enum variants,
      full paths stay legal, no scope fallback), or-patterns with the
      identical-binder law, guards with the never-exhaustive rule.
      Acceptance legs: bare prelude-variant and user-variant arms
      (nested inner match resolves by its own scrutinee); full-path
      cross-enum still legal; or-pattern union binder + binder-set
      mismatch rejection; guard selects conditionally, guard
      referencing bindings, guarded-only match demanding a fallback;
      fmt round-trips all three forms. Boot implements; the self-host
      mirrors. Landed 2026-09-26: resolution rewrites bare
      binder/variant names against the scrutinee's enum before
      covering or checking (payload field types drive the recursion);
      or-patterns emit once per alternative with the arm body gated by
      the matched flag; payload literal patterns now pin values (the
      tag alone used to decide, silently matching every payload); the
      matched flag gates every arm unconditionally (same-tag arms
      used to clobber each other); guards evaluate after the binders
      and never close exhaustiveness. **Self-host calibration wave,
      2026-10-04**: the mirror's §19 half arrived as a boot-parity
      rewrite of the pattern layer — recursive Pat (nested payloads,
      int/float/bool/string literal lanes), or-pattern alternatives,
      guards, bare-variant resolution against the scrutinee's enum at
      emission, bare binder arms taking the whole subject. The old
      mirror matched `Some(3)` on the tag alone (Some(4) took the
      arm), hard-wired bare Some/None to prelude tags (a user enum
      with a different variant order silently mismatched), read bare
      names as wildcards, and refused `|`/`if` arms outright. The
      gate's leg 4 then caught the rewrite's own bug: the bool pin
      read the lane marker instead of the value, both Some(bool) arms
      pinned 1, and every `while true` in the compiler's own source
      folded away — n26's law reborn through the rewrite, fixed by
      reading lit, and two arena-cap overflows (the string pool's
      fixed 2048, the arm table's fixed 12) grown on demand. n29 pins
      the program-level face; 674 suite cases and the 100-program fmt
      parity ride it.
- [x] **T3.10** Bare receiver in impl methods (§18): `self` = `*T`
      read-only, `mut self` = writable — the type inferred from the
      implemented type; fully-typed receivers stay legal; signature
      match (impl vs trait) keeps the receiver mut form. Acceptance:
      bare-self impl satisfies a trait, bare `mut self` writes through
      with the §18 gates, typed and bare forms mix in one impl, fmt
      round-trips. Landed 2026-09-26: the signature pass derives the
      bare receiver from its home (*T for a struct/enum target with
      the type's own parameters, the value for a builtin primitive),
      the typed form is accepted and never required, and fmt
      canonicalizes it away. The §18-gated acceptance legs ride
      T3.6's commit (the gates do not exist yet anywhere).
- [x] **T3.13** The in-boot wasm interpreter and in-process execution
      (opened 2026-09-27, ruled by the owner: an external runtime on
      the default path is not acceptable). run, fmt, and test are
      boot verbs end to end — fmt already is; run and test complete
      with the interpreter, no external tool on any path. Boot loads
      the binary module it itself assembled (T3.8 edge 3) and
      executes it in process. The interpreter's import surface IS
      the kernel's import surface, by law — the two grow in the same
      commit or the gate reds. With this task the kernel surface
      grows to fd_write, proc_exit, args (args_get/args_sizes_get —
      `rho run -- args` is dead today and comes back), and fd_read
      (stdin; the archive kernel had both, the 0.1.0 rewrite dropped
      them) — file tails (path_open &c.) still wait for std.io in
      Phase 4; clock/random/environ never exist (the determinism
      law). Interpreter-owned value/call-depth caps produce
      `panic: stack overflow` (exit 101) deterministically,
      replacing the wasmtime trap-grep translation in test.c/
      driver.c. `rho run`, `rho test`, and the gate's behavioral
      legs execute in process by default; the PATH dependency ends
      with the silent-failure family (suppressed assembler stderr,
      bare exit 1). Per-test isolation keeps §17: every case gets a
      fresh instance. Acceptance before the switch: the whole
      corpus, every suite, and the self-chain run 100% behavior-
      identical (stdout + exit) under the interpreter and wasmtime,
      floats bit-exact through the differential. Honesty note:
      interpreting the self-hosting chain is slower than wasmtime by
      roughly an order of magnitude; the reference legs carry the
      heavy runs during transition, and interpreter hot-spot work is
      a separate task if the wall clock ever demands it. After the
      switch, wasmtime survives ONLY as the short-term test-
      environment reference leg (ruling 2026-09-27), retired behind
      an explicit opt-in flag once the interpreter has carried the
      full self-chain plus one full gate cycle green. Production and
      site paths never reference it. Boot's own C stays portable
      enough to compile to wasm again — the archive's `__wasm__`
      build returns and the site embeds the real compiler — and boot
      exposes a capability face (which kernel imports and mechanisms
      it supports) so hosts and the course can feature-detect and
      flag what boot does not support.
- [x] **T3.14** The mirror source split (opened 2026-09-27): the
      self-hosted compiler is 15.9k lines across six files with
      emit.rho alone at 10.9k — restructure it into the module
      system it compiles: a lib.rho facade with lex/parse/check/
      emit/fmt as sibling modules (or subpackages), completing
      T2.x's "port module by module" spirit and making the compiler
      source the language's own showcase. Sequenced after T3.10's
      differential floor stabilizes — the split touches every file
      mid-adaptation otherwise. Acceptance: the differential floor
      does not drop, fmt over the whole source stays clean, every
      gate leg green, and the self-chain's multi-file source transit
      unchanged.
- [x] **T3.15** The package manager, in boot (opened 2026-09-27,
      ruled by the owner): `rho pkg` — init/add/install/lock/
      vendor/build over the module-system's own package law (a
      package is a directory behind a lib.rho facade; `std` stays
      reserved). Manifests, a lockfile, and vendored path+git deps:
      installed packages are vendored in-tree, so the bootstrap
      chain keeps building from vendored sources only
      (module-system §7 holds — the gate never touches a network).
      This is boot work, not mirror work: the verb compiles nothing
      itself, it fetches, pins, and lays out trees that `rho build`
      already consumes. Unblocks T4.3/T4.5 — the std packages become
      real, installable packages the compiler can consume.
- [x] **T3.16** The language server (ruled needed 2026-09-27): the
      archive's tools/lsp is the reference shape — the compiler runs
      as a worker, the LSP wraps its diagnostics and answers, a VS
      Code extension rides the same worker. Lands after the
      diagnostics context upgrade (anchor notes, candidate naming)
      so the server surfaces the checker's real face, not the bare
      one-liners.
- [x] **T3.17** Fuzz and sanitizer legs (ruled needed 2026-09-27;
      three complementary probes and the discipline that glues them).
      **Landed 2026-10-01.** robust/ is gate leg 2c (the deterministic
      leg: hand-written, readable hostility under wall-clock caps).
      The discovery leg returned as `tools/fuzz/gen.mjs` — the
      grammar-aware, seeded-LCG differential campaign, time-boxed and
      reproducible by seed number (gate leg 2d). The ASAN+UBSAN build
      (`make asan`) is gate leg 2e over robust/, the corpus, and the
      saved fuzz programs — and it caught a real one on its first
      gate run (`robust/wide_expr.rho`: the checker's unguarded
      recursion smashed the 8 MB host stack under ASAN's fat frames;
      the checker now carries CHECK_EXPR_DEPTH_CAP 4000, refusing
      cleanly at the parser's own bound — the bang_deep_ok suite case
      at 2017 levels pins the floor). The repro discipline is the
      corpus itself: the campaign's four finds are minimized and
      pinned as corpus cases n22-n25, each naming its seed in the
      header, graded on both compilers by gate leg 3 forever — seeds
      1 (f32 operand rounding), 26/40 (bool-vs-float hole
      classification), 7 (the divish $r scratch clobber), and 114
      (the fold's missing sign extension — a boot bug: the if-
      condition fold skipped the i8 wrap and read (-14)*(-32) as 448
      while the runtime read -64). Findings fed the counting floor:
      PINNED 118 → 122. Land early rather than late — T3.10's lesson
      is that the mirror calibrates against boot's broken faces, so
      every boot bug found before the mirror adapts to it saves a
      whole re-adaptation wave; this round proved the point twice
      (the f32 lanes and the fold signs both predated the mirror's
      adaptation).
- [x] **T3.18** The site's truth leg found the mirror's remaining form
      gaps (2026-10-01, opened with T5.1): the verify-site-examples
      mirror leg refused to ship a site example the mirror cannot take,
      and two legal shapes fell out of the tour's first draft —
      (a) a closure literal in a let initializer
      (`let add10: fn(i32) -> i32 = fn(x: i32) -> i32 { … };`) makes
      the mirror's own wasm panic `index out of bounds` mid-compile —
      a compiler-side memory bug in the EX_CLO init path, the worst
      class there is; (b) a call-on-call postfix (`make_adder(5)(0)`)
      falls to the parse-fallback counter and refuses. **Both closed
      2026-10-01.** (a) was `clone_fndef(prog.fns[0])` in
      emit_closure_value — the carrier template read the first
      non-main fn, and a program with ONLY main (main rides
      prog.main) has an empty fns table: index out of bounds. The
      carrier needs only its body; parse.new_fndef synthesizes it
      (the old clone deep-copied a body the next line overwrote
      anyway). The phase-probe hunt (eprintf marks at CV/LET/ST/GD/CF)
      also exposed the debug discipline trap: a use-after-free print
      reads freed bytes and names the wrong suspect. (b) was the
      postfix loop having no bare-call suffix — ident+`(` was eaten
      once in the atom, so the second `(` of `mk()(7)` had no owner.
      The loop grew the suffix with parse.call_value (the callee rides
      EX_CALL.lhs with an empty name), the emitter indirect-calls the
      parked (idx, box) pair through the $clN types the closure
      registered, and the checker's unknown-fn scan skips nameless
      calls. `mk()(7)` now compiles and prints 7 on both compilers;
      the two probes that found the gaps (tour's chain and the
      closure-init) are the regression shapes. The law-family residues
      both CLOSED 2026-10-03 (the residue integration): the §18
      call-site mut marker (residue/mut-call-marker — the mirror now
      pairs a marked argument with a mut root binding, boot's exact
      diagnostic, walking EX_FIELD/EX_IDX chains to the path the way
      boot's root_binding_mut does; derefs and temporaries are their
      own mut roots) and boot's stricter statement-match spelling
      (residue/stmt-match-spelling — a statement match's trailing ';'
      now refuses with boot's wording `expected an expression,
      found ';'`; the old "eats its trailing `;`" note in the
      fifty-fifth/sixth cuts was the mirror alone and boot-false).
      Both t13 pins ride the differential; PINNED 94 → 96. The
      integration also rebaked the seed canary (1,484,341 bytes):
      the mut-marker closure had changed the mirror and closed on
      make test alone, so the canary leg caught its stale pin — the
      gate's own law (re-pin in the commit that changes the
      compiler) applied at the merge.

## Phase 4 — kernel boundary and the std library

- [x] **T4.1** Kernel audit: the prelude contains exactly the
      mechanism-required set (Option/Result+`?`, to_str + format sinks,
      panic hooks, allocator + rc glue, string primitives, raw per-target
      syscall tails). Nothing else. The standing law: **the kernel grows
      only when a language mechanism grows.**
      AUDIT 2026-10-01 (the twin-prelude inventory, boot kernel vs the
      self-host's $w_* set):
      - CLOSED: the __ face (std.io's raw wasi window) rode boot only —
        a self-host compile of any io program emitted undefined calls
        under wabt's resolution-skip mask. The self-host now carries
        the three imports, the four wrappers ($w_fd_read_packed /
        $w_fd_write / $w_fd_close / $w_path_open), and __string_from
        as a build_string_pair face (the string-let binds the pair;
        the scalar lane rides the address). io scratch locals $sfa/
        $sfl joined every function prologue. n14_io_stdin pins the
        differential (floor 111).
      - DIVERGENCE, RESOLVED 2026-10-01 (user ruling: implementation-
        defined, prove equivalence): weak handles. boot = a handle box
        + a weak count at payload-16 (rho_weak_from/alive/payload);
        the self-host = the identity pointer, get() probes the rc
        header. Both allocators are bump kernels that never recycle
        ("keeps memory for 0.1.0 bring-up"), so the probe reads the
        dead header's rc=0 forever and the two are observably equal.
        The dangerous shape — die, then a same-size churn (the shape
        any recycling allocator would serve from the freed block),
        then weak.get() — is now a corpus case (n15_weak_reuse, floor
        112): both compilers read None after the churn and Some for a
        fresh node. The spec (§1.1) now declares the handle's own
        representation implementation-defined; the law is what
        weak.get() observes. The day either allocator starts
        recycling, this case goes red unless the weak header is
        honored.
      - DIVERGENCE, RESOLVED 2026-10-01 (user ruling): ?T layout.
        boot = flat 3-slot; the self-host = a heap box. The spec
        (type-system §9) now declares Option/Result memory layout
        implementation-defined — the spec pins behavior only
        (matching, comparability, propagation); both representations
        conform. Programs see one compiler, so nothing mixes today;
        the corpus would catch drift.
      - Strategy note: name-for-name diffs between the two preludes
        are the wrong lens (the fb_* format family vs the write_fd
        piece strategy is deliberate); audit by mechanism, then by
        observable behavior on shared fixtures.
- [x] **T4.2** `std` = the reserved in-repo directory; `use std.io;`
      LANDED 2026-09-27 from the parallel wave (t42 worktree): a use whose first segment is std resolves against the reserved in-repo std/ tree (anchored at the boot/-marked repository root — the provisional ruling, recorded); ten stdrule fixtures.
      resolves by the single rule (first segment `std` → the reserved
      directory); every other `use` stays two-base relative.
- [x] **T4.3** std.collections: Vec and Map as real packages (extracted,
      FIRST WAVE LANDED 2026-09-27 (t43): Vec and the ordered Maps (sorted parallel arrays under binary search; ascending iteration is structural) behind a facade; literal extraction and compiler consumption follow.
      not copied, from the compiler's own source; the compiler consumes
      them afterwards). Map iteration order is **deterministic and
      documented** (D3).
      CONSUMPTION PREREQUISITES (2026-10-01 probe: std.collections
      through the self-host): two module-forms fixed in the self-host's
      loader — (1) `pub use X.item;` (alias-less re-exports) now loads
      the head module and sells under the item's own name; (2) the
      suite verb `test "..." { ... }` parses-and-drops in normal
      builds (boot checks the body for `rho test`; the emitted program
      never carries it). The probe then stalls on the REAL blocker:
      **generic structs are boot-only** — `pub struct Vec[T]` and the
      per-instantiation methods (`fn Vec.push(self: *Vec[i32], ...)`)
      hit 5 parse errors in the self-host's parser, which knows only
      fn-level type parameters. Porting generic structs (parse + the
      struct table keyed by mangled instantiation + field/method
      substitution in check and emit) is the wave that unblocks both
      the compiler's own consumption of std.collections and any
      std-using corpus case through the differential.
      WAVE IN FLIGHT (2026-10-01): the 5 parse errors were bare
      prelude-variant patterns (Some(x)/None now normalize to
      Option./Result. in parse_pat). The probe then climbed — loader
      forms (fixed), the sibling-call qualifier (module-qualified
      mcalls rewrite to plain qualified calls), sym_name mangling
      brackets, ginst_method_target suffix-matching qualified methods,
      generic-struct method templates skipped from the drain (boot's
      ngparams law — only clones run), the clone receiver's
      star-blind substitution, and the full-text binding for
      instantiation params. The probe now ASSEMBLES and runs its Vec
      half; the remaining gap is a RUNTIME one — the clones' tail
      field stores (self.data[self.n] = v; self.n += 1) emit empty
      (the clone's ST_FSTOREs render nothing), so n never advances
      and get(0) panics. RESOLVED same night: the store's base lookup
      rode st_index, whose new star-strip let the pointer receiver
      match the BY-VALUE arms — the param loop copied the whole struct
      into a scratch frame (self.n += 1 advanced the copy) and the
      annotated pointer-let bound a value frame (p.sum() read a
      const-0). The pointer spellings now win the arm ordering
      (is_ginst_ptr before the by-value faces; the by-value faces
      reject pt[0]==42) and st_index instantiates starred spellings.
      The probe's Vec half runs end to end (push/get/len through the
      clones). RESOLVED same night: both map-half gaps were the same
      lookup — a method's return type resolved only by the exact
      receiver spelling. method_rtype now resolves through the
      instantiation (suffix-matching the qualified template and
      substituting), and scrutinee_boxes/optpt_of_scrutinee/the
      boolish classifier all route through it — the probe's map half
      matches boot byte for byte (insert/get/contains/remove, option
      matches, bool words). The CHAIN is the one red leg: the child's
      self-compile panics with an OOB in en_vtag — one of its four
      unguarded callers (the Enum.Var constructor emissions, the
      box-face binder walk, or arm_cond) now meets ei=-1 on a
      construct the wave introduced. RESOLVED: the enum accessors
      guard ei<0 (en_vtag reads tag -1, en_nslots/en_vslots 0) and
      the child now completes. The chain's red moved to the
      determinism law: 17 functions diverge, rooted in an
      option-binding face — NARROWED (fmt_line's raw witness): a
      NESTED match whose inner arm condition compares the loaded tag
      to **-1** in v2 vs **1** in v1 — the grand's arm_cond folded
      `tag == en_vtag(ei=-1)` (the new guard's return) for an arm
      whose pattern enum did not resolve, so the arm can never match
      and the whole tail after it vanishes. The trigger construct is
      a match whose arm pattern's enum fails en_index — most likely a
      module-qualified enum pattern whose canonicalization misses
      under one of the wave's loader/qualifier changes. The
      const-pattern fix (arm_cond resolving qualified consts through
      the consts table) was correct but not the root — the
      bin_width witness refines it: the two levels disagree on a
      ?*Expr FIELD OFFSET (v1 loads the scrutinee `e.lhs` at +16 —
      the name field's slot — where lhs lives at +40 of the Expr
      layout) and v2 folds the Some arm's payload to an empty-string
      pair (0,0). I.e. the field-offset/field-type resolution for a
      generic struct's ?T fields reads a DIFFERENT struct entry at
      the two levels — REFINED to the binder: in the grand's Some
      arm, the binder `l` has NO recorded pointer lane (its field
      read folded to the scalar-lane const 0, and parse.EX_LIT = 0
      made the comparison 0==0), i.e. the binder binding from the
      option box never took env_bind_ptr for this shape — prime
      suspects: parse_pat's binder collection after the
      ambient-variant restructure (the dotted vs shared path), and
      the box-face binder walk's payload-text resolution (5944
      region) for ?*Expr payloads. SHARPEST WITNESS (post const-fix,
      fmt_line): the child emits the Some arm as `tag == 1` with real
      payload loads; the grand emits AN ARM as `tag == -1`
      (const_i64's miss return) with a folded-zero body — in the
      grand's runtime SOME arm's ename is neither Option/Result nor a
      registered enum, though the source spells Option.Some/None.
      Same source, same arm_cond code — the resolver/exports TABLE
      STATE differs between the v1 and v2 runtimes. MEASURED (the
      ename + mk_pat probes ran the full chain): the child's runtime
      builds 468 patterns (mk_pat) and prints 438 Option.Some/None
      parses; the grand's runtime prints ZERO of either yet arm_cond
      sees 436 kind-2 patterns
      spelled `Some.Some`/`None.None` (ename duplicated from vname) —
      the grand's patterns do not come from its parse_pat run.
      clone_expr SHARES patterns (pat: src.pat) so clones keep their
      ename; the mangler is elsewhere. MEASURED (the mk_pat probe ran
      the full chain): the child's mk_pat fires (kind-2 builds land,
      438 kind-2 parse prints); the grand's mk_pat prints ZERO yet
      arm_cond sees 436 Some.Some/None.None — the grand's patterns
      come from a builder that is not mk_pat and not parse_pat.
      METHOD NOTE: wat function extraction by the first `  )` line
      TRUNCATES folded-form bodies — use balanced-paren scanning
      (verified: parse_pat is genuinely 865 complete lines at both
      levels) — and treat wat text-grep as unreliable (data segments
      escape inconsistently). RESOLVED 2026-10-01: the paradox was an
      ARTIFACT — every cross-level probe comparison compared runs of
      stale artifacts (a grand assembled from a pre-probe wat, a
      "child" that was actually the mirror). The real red was
      ordinary: leg 4 diverged at clone_expr's first `match e.lhs`
      (v1 tag 1, v2 tag -1) and v2 didn't even assemble (8 undefined
      `call $u_len` in emit_stmts). ROOT (bisect: fd8f3bd): a mut
      string/slice let over another name SHARED the source's live
      slot pair (emit.rho's ST_LET EX_VAR path), while boot's NT_LET
      gives every let a fresh register — the ambient-variant
      restructure then added `let mut vt/en: string = name0` to
      parse_pat, and the in-place ST_SET rebind made vt, en, AND
      name0 one slot pair: p.ename = en read vt's dotted text, so
      every pattern spelled bare `Some.Some`, arm_cond fell to
      const_i64's -1, and the len() classifications fell through to
      the undefined $u_len. FIX: a MUT let materializes its own pair
      (copy the source's slots); an immutable view keeps the shared
      binding. Leg 4 green, suite/corpus green.
      STD DIFFERENTIAL PIN (2026-10-01, user ruling: prove, don't
      assume): n16_collections puts std.collections through the
      differential — the loader's MODS bake now carries the std/ tree
      (repo-relative paths), and the program pins the Vec and Map
      surfaces through the clones (push/get/contains/remove/pop, the
      ?V option matches, bool words, D3's ascending-key iteration).
      Four emitter gaps fell out, all the same family (a method
      return resolved only by the exact receiver spelling): (1) the
      ST_LET slice-call gate read EX_CALL only — v.view() bound
      scalar and len(xs) fell through to an undefined $u_len (the
      same symptom as the chain red above); (2) the ST_LET enum arm
      and expr_enum_type's method arm missed ?V returns (norm_enum
      does not read the ? spelling — ?T is Option sugar, §9), so a
      let bound from m.insert(...) fell scalar and its match loaded
      the box pointer as the tag and matched nothing; (3)
      build_string_pair's method arm resolved the declared return
      star-blind and targeted the template — it now routes through
      method_rtype and ginst_method_target; (4) ginst_method_target
      cloned the FIRST overload sharing the method name (StringMap.
      insert has one concrete overload per value type — the [string]
      body landed under the _i32 key, 5 params vs 4) — the overload
      whose receiver instantiation matches the call now wins, and
      method_rtype's scan sees bare-registered methods. Floor 113.
      REMAINING for T4.3: the literal extraction review (std.
      collections vs the compiler's own growth tables) and the
      compiler consuming the packages.
      CONSUMPTION WAVE 2 LANDED (2026-10-01 late): std grows Vec[bool]
      (push + the vec_bool factory + the facade re-export — the facade
      test names every binding, so the new line is pinned), and the
      match-binder hole classification learns bool payloads: a
      Some binder over a ?bool scrutinee records its type in the env,
      so printf spells true/false through the binder (a bare binder
      once printed the box tag — 1/0 — because neither the binder nor
      any let chain recorded bool). The build_string_pair gate and
      hole_boolish route share match_is_bool_payload (the ?bool
      scrutinee via method_rtype), so a bool-payload match is never
      claimed as a string build. Suites 573, corpus 113, GATE all
      legs with the std-consuming sources on the chain. NEXT: the
      no-grow const table (cnames/cvals/... fixed 64, unguarded —
      parse.rho itself already rides 33/64) migrates to Vec in wave 3;
      the enum variant tables (vnames/vslots fixed 16, vtpool 128 per
      8-payload slot — a 17-variant enum overflows) ride the same
      wave; pointer-element tables wait for the boot capability wall
      (call-site generic naming) to lift.
      CONSUMPTION WAVE 3 LANDED (2026-10-01): parse()'s accumulation
      tables grow — cnames/cvals/cstatic/ctypes/cmuts ride std Vec
      (vec_str/vec_i64/vec_bool, grow *2+1; the old fixed-64 arrays
      were written unguarded and parse.rho itself rode 33/64), and
      the enum tables follow: vnames/vslots ride Vec, vtpool stays a
      local array with a per-variant grow (its writes are sparse —
      slot v*8+p — and Vec.set cannot extend). cexprs stays a local
      doubled array: []*Expr cannot ride the package Vec while the
      boot capability wall stands. First-draft green: the chain ran
      LEG4 GREEN with no re-spin. Corpus 113 → 114 with
      n17_const_flood (70 chained consts past the old cap, an
      18-variant enum with a two-payload variant at index 16; boot
      strictness pinned in passing — main must return i32, statics
      are always `static mut`, the last enum variant takes no
      trailing semicolon). NEXT for T4.3: pointer-element tables wait
      for the capability wall to lift (a boot evolution item); the
      compiler consumes std for its value tables everywhere that
      matters today.
      CAP SWEEP + EXTRACTION REVIEW (2026-10-02): a whole-compiler
      sweep for the const-table bug class found six more sites —
      traits (fixed 16), structs (32), enums (32) wrote their tables
      UNGUARDED (OOB corruption class), and the use tables (32), the
      module-canon table (16), and the export tables (128) silently
      DROPPED/REFUSED on overflow (miscompile-by-omission class: a
      dropped use, a reloaded module, a name unknown to the checker).
      All now grow on demand under the fn-table law, and
      n18_table_grow pins the declaration-table growth through both
      compilers (33 structs / 17 traits / 33 enums, first and last
      entries constructed, matched, and called). Floor 115. THE
      EXTRACTION REVIEW (T4.3's other stated remainder) closes with a
      verdict: Vec is the genuine extraction — its growth idiom is
      the compiler's own tables with one documented divergence (std
      grows *2+1 because its factories accept capacity 0; the
      compiler's tables never start at zero and grow *2); the ordered
      Maps are NOT compiler extractions — the compiler has no
      binary-search literal — they are new code written under the D3
      order law (sorted parallel arrays, binary search, ascending
      iteration), with strutil carrying its own documented reasons
      (the len-overload ambiguity; boot's `<` comparing data
      pointers, not content). Every divergence is documented in the
      package headers themselves; nothing to reconcile. T4.3's
      remaining scope is now exactly the capability wall.
      THE WALL LIFTS (2026-10-02, the product-language ruling):
      call-site type arguments — `name[T1, T2](args)` — land in both
      compilers, and the compiler can now name package generic types.
      Boot: the grammar (a '[' after a callee opens the type-argument
      list when the matching ']' is followed by '(' — rho has no
      index-then-call; both the plain and the dotted call spell it),
      the seeded instantiation (a factory's T rides only its return,
      so argument inference can never bind it), and the DEEP
      substitution that maps make([]T)/new Vec[T] inside an instance
      body (the bare-param check missed composite spellings — the
      make-in-factory gap). The method diagnostic's mut-blind retry
      learned the same substitution (the generic push once reported
      "expected T, found string" where the law says the element
      type). The self-host rides the same law spelled the new-T[A,B]
      way: the targs fold into the callee's name text and the
      qualifier (both walkers), the checker, and the emitter's
      instantiation key split the same spelling; method returns off
      instantiated receivers resolve through method_rtype (a bare
      fn_rtype_of once collapsed every field read off a targs call to
      constant 0), and hole_boolish learned EX_FIELD (a bool field
      printed 1/0 — the bare-tag law again). std grows the generic
      factory vec_of[T], and the twelve concrete push/insert
      overloads collapse into one generic form each — the collapse
      the old capability note promised the day make substitutes.
      Corpus 115 → 116 with n19_targs: the compiler-consumption shape
      (vec_of[*Pt] over a type the PROGRAM owns, the generic push
      serving it, inference still binding where it can). Chain LEG4
      GREEN; suites 573/0; gate green. NEXT (W4): the compiler's
      pointer tables consume vec_of — the payoff that was waiting on
      this wall.
      W4 LANDED — THE PAYOFF (2026-10-02): parse()'s accumulation
      tables consume the package Vec through explicit type arguments
      — fns/nodes/structs/enums/traits/cexprs ride
      `collections.vec_of[*T](cap)`, the use tables ride
      vec_str/vec_bool — and every local growth block the cap sweep
      added retired: push IS the growth law now. The chain carries
      six cross-package instantiations (Vec[*FnDef], Vec[*Stmt],
      Vec[*StructDef], Vec[*EnumDef], Vec[*TraitDef], Vec[*Expr])
      through all three levels byte-identically — the compiler eats
      its own extraction through the deepest differential there is.
      T4.3 is CLOSED: extraction, consumption, and the wall are all
      on the record. Suites 573/0; corpus 116; gate green.
      WORKTREE CONSOLIDATION (2026-10-02): the twelve parallel
      worktree branches all sat 0-ahead of master — the std waves
      (T4.2–T4.6) and the t35 protocol landed through their reviewed
      merges, so their worktrees retire with their branches. The
      ecosystem deliverables did NOT land — they commit now:
      plugins/prettier-plugin-rho rides master (the vendor refreshed
      to the current compiler, the artifact rebuilt through the
      chain, the generation pin bumped, and the wasm import object
      grown for the kernel's whole wasi tail — the std-era sources
      pull fd_read/fd_close/path_open where the old artifact carried
      only fd_write/proc_exit). Its identity law finally runs LIVE
      against a real boot — the worktree never had one, so the test
      skipped silently — and that exposed a real language gap: THE
      COMMENT REPLAY LAW HAS NO RHO-SIDE PORT. boot's fmt records
      every comment (fmt.c's cmt_flush/cmt_tail) and replays them by
      line; fmt.rho drops them, and the fmt-self parity fixtures are
      all comment-free, so no repo leg ever compared a comment. The
      plugin's identity suite names the debt (36 corpus files skip
      with the reason; a guard test keeps the count visible). THE
      PORT'S DESIGN, for the wave that lands it: the lexer records
      (text, line) into a module-level table reset per lex() call
      (boot's g_cmts design); Stmt and the five decl structs grow
      line/end_line stamps (mk_stmt is the single construction point;
      parse_stmt stamps end_line from the last consumed token; the
      decl sites stamp from their branch token); fmt replays at
      boot's three site families (before each declaration, above
      block closers, at the file tail) with the same monotonic
      cursor. Witness when landed: the plugin's owed set drains to
      zero and its guard test retires.
      THE COMMENT REPLAY PORT LANDED (2026-10-02, the same day): the
      lexer records the file's comments beside the tokens (lex.scan
      returns Lexed { toks, ctexts, clines } — two parallel arrays,
      the const-table idiom, because a cross-module struct type
      cannot spell itself in the consumer's annotations); Stmt and
      FnDef carry line/end_line stamps (mk_stmt is the single
      construction point; the parse_stmt wrapper stamps from the
      first and last consumed tokens; the fn and const branches
      stamp their declarations — consts ride two more parallel Vec
      tables); fmt walks a monotonic cursor over the tables at
      boot's site families (above each statement and declaration,
      above block closers by the block's last statement's end line,
      above main's signature — main is not a prog.fns member, and
      without that flush the file header landed inside main's body —
      and at the file tail). Two boot behaviors the port had to
      match byte-for-byte: a declaration's same-line tail comment
      prints on the NEXT line indented and takes the blank
      separator's place; a fn's closing-brace note stays on the
      brace's line. The fixpoint law grew one allowance: a tail
      comment cannot survive its own repositioning (boot's own
      roundtrip drifts identically), so run-fmt-self compares the
      SECOND reformat — the fixed point, reached in bounded steps —
      and tests/fmt-self/comments.rho pins the whole shape set.
      The plugin's owed set drained to zero: the identity suite
      runs every corpus file (comment-bearing ones included) live
      against boot byte-identically, the skip machinery and its
      guard retired, and the vendored fmtmain lexes through scan()
      with a rebuilt artifact + bumped generation pin. The plugin's
      capacity-boundary test now sizes its pad with an
      already-canonical tail: the old premise (output smaller than
      input) held only while comments were dropped.
      T5 PROBE — THE SITE REFRESH HIT THE REAL DESIGN GAP (2026-10-02
      night): the app's rho.wasm (0.4.0, Sep 25) predates the whole
      restart era, and the current mirror's build face is BAKE-ONLY —
      SRC/MODS are build-time consts (§14: the self-host has no
      files), the WAT goes to stdout, and the runtime CLI faces the
      old artifact carried (argv + path_open reads of /main.rho) are
      retired. The site needs runtime compile; the gap is a rho-side
      wave, scoped by tonight's probe: (1) main.rho grows the app
      face — when the host writes /main.rho, compile THAT (the verb
      rides a /mode marker: build default, check, fmt); the module
      tree rides a second baked const MODS_APP (the site's artifact
      bakes the std tree there; the chain's own build leaves it
      empty, so §7's law stays untouched). DRAFTED AND PARKED: the
      face works under wasmtime --dir .::/ mappings but tripped TWO
      PRE-EXISTING EMITTER HOLES on its first real exercise — (a) a
      pair-returning fn's `return match {...}` (std/io's
      IoError.to_str — the string match rendered as a pair, then the
      return rode one slot; rtype_is_pair's sret path needs the
      match-as-pair face), and (b) an Option/Result binder used as a
      string CALL ARG rides scalar ($_start's call $u_app_compile got
      [i64] where the pair ABI wants two) — the optpt fix of the
      night (optpt_of_scrutinee's Result arm, committed inert) fixed
      the SCRUTINEE-side payload read; the binder-lane/call-arg pair
      is the remaining half. (2) the app bridges the mirror's WAT
      stdout through wabt (drafted, works — wabt assembles the
      subset; the repo's own wat2wasm law, never a second assembler).
      (3) wasm-opt RETIRES for rho artifacts: every level (-Oz/-O/-Os
      with bulk-memory+multivalue, --enable-all) miscompiles or
      strips the current artifact (the io/args imports vanish, the
      CLI goes silent) — the raw mirror artifact (1.42 MB vs the old
      2.47 MB) is the shipping form until §13's in-compiler
      optimizer exists. NEXT WAVE ORDER: (a) the two emitter holes
      with corpus witnesses, (b) main.rho's app face re-landed green
      through the chain, (c) the app's compile() bridge + raw
      artifact + the lesson corpus re-pinned, (d) RHO_VERSION bump.
      rho-lsp, vite-plugin-rho, and tools/bench stay ON THEIR
      BRANCHES (in-flight, not superseded): their uncommitted state
      is committed branch-side (no node_modules), the worktrees
      retire, and their known-red suites are the landing work —
      rho-lsp 109/114 (runner-bake ordering, refusal parsing,
      surrogate clamping), vite 77/83, bench 69/84 (the report
      renderer). gap-tests likewise keeps its branch (GAPS.md, the
      gap-fmt goldens, robust-gap, run-fmt-gap.sh) pending its own
      landing pass against master's Makefile.
      CONSUMPTION WAVE 1 LANDED (2026-10-01, after the deep hunt):
      parse_pat's binders ride std.collections' Vec[string] — the
      fixed cap (make([]string, 8), never grown) is gone — and the
      chain's MODS bake carries the std/ tree. Two findings first
      recorded as blockers, now resolved:
      - CAPABILITY WALL (still standing): the compiler's
        pointer-element tables ([]*Stmt/[]*Expr/[]*Tok…) cannot
        consume std Vec yet — boot cannot name a package's generic
        type at a call site, and std cannot reference compiler types
        (Vec[*Tok] is unspellable on both sides). String/int tables
        and a keyword map can consume today. Wall lift = a boot
        evolution item (call-site generic naming, or make's type-arg
        riding instantiation).
      - LOADER GAP, FIXED earlier this round: a std-using MODULE
        through the baked-MODS path half-qualified facade re-exports;
        the module-load qualifier now lands on the export table's
        registered target (boot's disk path handled the shape all
        along).
      - THE PARADOX FINGERPRINT RESOLVED: the leg-4 red under the std
        bake was NOT corruption — gstruct_method_skip's
        `base = fd.name[i..j]` is a slice view over a STRING FIELD,
        and the ST_LET EX_SLICE2 field-base deduction only fired for
        "[]T" fields. A string field fell to the env path, which
        resolved the base through the env by the FIELD's NAME
        ("name", unbound → the encoded local 0 = the fd pointer),
        built the view at fd+lo*8, and read its "elements" as i64
        pairs from inside the FnDef struct (b0=856896): st_index
        missed, skip=false, the grand emitted the canonical templates
        (+50 fns), and the probe's own eprintf with that view as a
        string hole printed garbage/zero — the zero-length-write
        phenomenon. The expression-position twin already handled
        string fields; the ST_LET path now does too (byte stride 1,
        kind 1, no element text). The dormant-bug lesson: the
        compiler's own two-segment fn names kept the view code
        unparsed-by-this-shape until std's three-segment names
        activated it.
- [x] **T4.4** std.io: read_line, file read/write wrappers over the raw
      LANDED 2026-09-27 (t44, reviewed and merged): fd_read/fd_close/path_open + four raw wrappers in the wasi tail; the prelude carries std.io's private __ window; read_line/read_file/write_file over Result; the verb gains // in: and --dir; the prelude's generic-instance chains reset per program (a real in-process compile fix the review caught); eleven io fixtures.
      tails.
- [x] **T4.5** json — the first real std package, written in rho and
      LANDED 2026-09-27 (t45, reviewed): parser + serializers + the exact-decimal number core as std.json; the review fixed the validator's two off-by-ones and the grain-literal confusion; boot's float compound assignments take their own load/store arms; seven fixtures.
      installed/consumed through `rho pkg` (T3.15): encoder + decoder
      + the deterministic-map story it needs from T4.3.
- [x] **T4.6** (library, non-blocking) utf-8 package: code-point
      LANDED 2026-09-27 (t46): the utf-8 package — one total panic-free decode step, strict validation, iteration and friends; six fixtures, the std-import pending promoted.
      iteration and friends — a package, never the kernel.

## Phase 5 — sites and course

- [x] **T5.1** Language home (site/): hero, tour, playground, spec
      reader — rebuilt around the new compiler; playground = compile on
      the main thread, execute in a worker (V8 worker-context miscompile
      still unreported — minimize and report upstream); phase-split caps
      120/20/10 s; fat functions to linear memory (`w_memmode`); deploy
      asset = one generation past the seed + `wasm-opt -Oz
      --enable-bulk-memory` — wasm-opt stays the site-asset shrinker
      until the in-compiler optimizer (§13) demonstrably matches its
      effect, then retires. Rulings 2026-09-27: the archive site
      (tour, real-compiler playground, spec reader) is the floor to
      EXCEED, not a template to trace; tutorials may run either
      compiler — boot or the self-hosted one — and anything boot does
      not support is surfaced as a hint from T3.13's capability face.
      **Landed 2026-10-01** (unpushed; push = deploy, the owner's call).
      Four pages, zero framework: hero (tour output pinned by make
      test), a seventeen-chapter tutorial (sixteen live editors, every
      example run-capable in place), the playground (examples, live
      check squiggles, fmt, stdin with the terminal row, shareable
      #code= hashes), the spec reader over committed copies of the four
      documents. The compiler artifact = the mirror app face shrunk by
      `wasm-opt -Oz --enable-bulk-memory --enable-multivalue` (843 KB →
      418 KB, smoke-probed after the shrink); WAT assembly rides wabt in
      the browser — one assembler concept, the same text the repo's own
      chain feeds wat2wasm. The editor bundle rebuilds from source via
      esbuild, and its completion tables now speak the real surface
      (std packages, weak[T], the operator traits; null is not
      offered). The truth law: tools/verify-site-examples.mjs runs
      every example through boot AND the self-hosted mirror in make
      test — the site's copy cannot say anything the compilers do not.
      Walkthrough (real browser): tour auto-run exit 0 with the pinned
      stdout, the hello chapter compiles and runs in place, the reader
      renders. Two finds from the mirror leg are ledgered as T3.18.
- [x] **T5.2** Course (the bilingual app): all live blocks re-pinned to
      the new language (suite 133/138 green, 5 skipped on the absent
      bench record); the honest-limitation notes rewrite (`?T` non-null
      taught as the one true absence form — every `null` teaching face
      rewritten, `intrinsics.slice_string` retired from the course,
      enum tags taught as declaration order); the editor sources
      vendored into the app (the rewrite retired the submodule's site
      assets). The runtime face grew what the course exercised: the
      absence faces, the panic prefix, the bool match, the nested
      Result[?string, E] read_line law, the wrap-safe allocator, main's
      early return, and the dyn method read.
- [ ] **T5.3** (owner's call, do not self-deploy) add the deploy
      workflow and deploy the course site.

This phase's ecosystem delivery — the vite plugin and the prettier
plugin — is specified in [docs/ecosystem.md](docs/ecosystem.md): goal,
dependencies, shape, acceptance law, npm version policy. They start
only when their listed dependencies close.
**LANDED 2026-10-04** (packages/vite-plugin-rho, packages/prettier-plugin-rho):
each embeds its compiler wasm and pins the generation it was baked
from; the vite plugin's wasm is byte-identical to the CLI's on EVERY
programs-tier program — the runtime module channel (the app face reads
/mods, 2026-10-04) closed the package-module gap, so the three former
refusals (032/107/t09) compile plugin-side byte-exact too — and the
prettier plugin is byte-faithful across the whole tier with the
fixpoint law holding — the formatter-gap pin closed at zero.

The post-freeze ecosystem pair landed the same day, owner-approved:
tools/bench (docs/ecosystem.md §1) — the compile/exec/chain trend-line
harness, 76 tests green, a real --iters 1 run over the corpus clean;
packages/rho-lsp (§4) — the language server over the canonical
boot-bake + bare-wasmtime embedding face, diagnostics/hover/definition/
completion/format plus the §18 mut hint, every request time-capped,
the degradation latch and the generation pin (startup re-hash, inert on
drift) tested — 114/114 and both tsc configs clean; its generation
artifacts re-pin with `npm run build:generation`.

## Phase 6 — freeze and 0.1.0

- [x] **T6.1** Full gate green: every leg, corpus differential, suites,
      fuzz, both sites building — all on the wasm self-hosting loop.
      **Green 2026-10-01.** The gate runs twelve legs on every change
      (source build, selftest, fmt × 2, the suites 573, robust, the
      fuzz differential 150/150, the ASAN+UBSAN sweep, the corpus
      differential 122, the self chain mirror→child→grandchild, the
      seed canary, diagnostic parity) — the fuzz and sanitizer legs
      landed with T3.17/T3.3, four real bugs and the depth-guard find
      in that wave, and the assignment law landed with the site's
      mirror leg. Both sites exist and build: the language home (T5.1)
      and the course app (T5.2, prerender verified on every make).
      T5.3's deploy workflow + push remain the owner's call.
- [x] **T6.2** Tag `v0.1.0` — the one and only version. Release zip:
      `rho-0.1.0-wasm32-wasi.zip`, binary named `rho.wasm`, SHA256SUMS,
      English RELEASE.md. Push/tag/deploy timing belongs to the owner.
      **Landed 2026-10-01** (tag local, unpushed — push = publish, the
      owner's call). `tools/release.sh` assembles everything from the
      repo at the tag: the compiler is the app-face build (std baked
      in) with NO wasm-opt — the shrinker is the site-asset law, and a
      foreign wasm-opt version would break the published byte hashes;
      the release binary stays gate-reproducible byte-for-byte. Zip =
      rho.wasm + README (the RELEASE) + LICENSE; smoke probe runs the
      greet through the extracted, zipped binary before the sums are
      printed. The zip is written by hand (node, fixed timestamps,
      fixed order) after a rebuild-with-`zip` came out with different
      bytes — the archive stores mtimes — so the published SHA holds
      on any machine, any day: two consecutive runs hash identically.
      Local artifacts land in build/release/. The retired
      world's local-only tag series (v0.1.0 … v0.2.1+, pointing 303
      commits back at the pre-rewrite language, never pushed — origin
      carries no tags) is cleared so the name means what §10 says:
      one version, no other tags.
- [x] **T6.3** **Freeze.** boot and the language freeze together. From
      here the language grows no more; 0.1.0-era growth is libraries
      (utf-8, collections, io, net) and tooling quality (operand-stack
      emission, string pooling, linear-scan register allocation, escape
      analysis / rc-pair elimination — ordering decided when the freeze
      lands; the §13 in-compiler optimizer's finish line includes
      retiring binaryen from the site pipeline once its effect is
      matched). **Frozen 2026-10-01 at the v0.1.0 tag** — the state
      spec.md §10 has promised all along (one version, no evolution
      after the freeze); the ledgered follow-ups (T3.18's two form
      gaps aside) are now std/tooling work, never language surface.
      **The tooling backlog landed 2026-10-04** (see T2.x): operand
      stack, string pooling, live-range register allocation, literal
      retain elision, the consolidation sweep — with wasm-opt kept on
      the site pipeline (the chain product is 2.84× the wasm-opt
      product; the retirement bar stays unmet, honestly).
- [x] **T6.4** Corpus dissolution (after T6.3). corpus's historical
      role — behavioral memory of the pre-rewrite language — expires at
      the freeze; spec + suites own truth from there. Retire the
      `corpus/` directory by the three-way split: cases the suites
      already cover die; good teaching programs promote to `examples/`
      (user-facing, still gated so they cannot rot); whole-program
      interaction pins move to the suites' programs tier and the
      differential re-points there. What never retires: the
      whole-program integration layer, the differential base, byte
      goldens, the examples — only corpus's unanchored positives-only
      form retires. **Done 2026-10-01** (122 → 14 + 14 + 94): fourteen
      smoke shapes died under confirmed suite holders (divrem_trunc,
      int_wrap, labels, defer_exit_paths, bool_condition,
      if_match_expr — and printf itself, the body of every suite
      case); fourteen teaching programs promote to `examples/`, gated
      by tools/verify-examples.mjs in `make test`; ninety-four pins
      re-anchored as `tests/suites/programs/` — the textual goldens
      became `// out:` headers the suite verb judges (93), and the one
      byte golden (n11, non-UTF8) kept its `.out` beside the re-pointed
      byte runner, because a text header cannot carry arbitrary bytes.
      The pin is the tier's size: the differential re-points there and
      reads 94. The verb grew `// rawout:` (the expected stdout tail
      without the trailing newline) for programs whose last printf
      deliberately ends mid-line — the old corpus runner compared
      `$(…)` on both sides and silently stripped that difference; two
      cases (100, n11) had been pinned fuzzier than they ran. The
      canary's SRC was the retired 001_hello, so its exact bytes moved
      into gate.sh — the canary input never changes, only the compiler
      it bakes. run-corpus.sh (the archive-sweep runner) retires with
      the corpus; run-corpus-repo.sh re-points as the byte-golden
      runner; the spec's conformance map and docs re-point with it.

## Phase 7 — after 0.1.0: native compilation, in the std library

Not part of 0.1.0. Native compilation is a std-library-era workstream;
the language does not change when it lands.

- [ ] **T7.x** Native backends as std-library components: arm64-mac
      (Mach-O + ad-hoc codesign), amd64-linux + arm64-linux (static ELF,
      raw syscalls), own assemblers. The archive branch's bug ledgers
      (asm64/asm86 ten-pit tables, Mach-O kernel gates) are required
      reading — same iron, **read them, never copy them**.
- [ ] **T7.x** The native ring (self build, structure check, --version,
      native-compiled wasm hello, ring child, child rebuilds corpus) as
      a **time-capped step on an ephemeral CI runner** (background +
      poll + hard timeout; timeout = red; a wedge dies with the VM).
      Local gates stay build-only. Linux crossings stay build-only
      until a container exists. MCU remains a future backend: one
      emitter + assembler + image-writer triple, nothing in the
      language.

The post-freeze ecosystem — the LSP and the compiler benchmarks — is
specified in [docs/ecosystem.md](docs/ecosystem.md). The LSP starts
only at the freeze; the benchmarks ride the T3.1 gate's harness when
the numbers can mean something.

---

## Phase 8 — mirror parity: the shipped face catches the gate

The browser artifact (libs/compiler in its app configuration) lags the
boot reference on a set of faces probed on 2026-10-06 by the
rho.ningh.org course audit. **The root cause is architectural**: the
mirror's checker is name-level only (check.rho resolves names, fns,
methods, labels, markers — it never computes an expression type), and
the emitter therefore rides untyped lanes. Boot's check.c/check2.c is
the full type system the mirror never grew. Every item below is
**boot-green and mirror-red today**; the equality/mut/aggregate faces
all sit on the same missing foundation, so the campaign's real first
milestone is a type-checking layer in the mirror (boot's checker is
the spec; port it law by law) — the items below then become local
diagnostics and width fixes on top of it. Each landed fix carries its
differential program (tests/run-mods-diff.sh is the module-face leg;
run-diff.sh the single-file one). The course site carries honest warn
notes pinned to this ledger; when an item lands, those notes free back
to their positive forms.

- [ ] **T8.1** Element-wise struct `==` (§10): the mirror compares
      `*T`-carried structs by reference identity — the gate's
      `eq_struct_nested_value_test.rho` prints `eq=false` through the
      app face, `eq=true` through boot.
- [ ] **T8.2** Aggregate-valued `match`/`if` arms in let position: the
      mirror silently binds the WRONG arm (a struct-valued match yields
      the default arm's value — worse than a panic). Add the gate pin
      boot currently lacks.
- [ ] **T8.3** The §15 operator-trait face: `impl Eq` is silently
      ignored (== falls back to reference identity), `impl Ord`
      dispatches `lt` to identity (only `>=` is accidentally right),
      `impl Hash` emits a call to an undefined function (wabt refuses
      the WAT).
- [ ] **T8.4** §2 item-level imports: `use math.Vec2;` resolves at
      mods_load as a MODULE path and dies "unresolved" — the item face
      (modsys `item_type_import`, boot-green) is missing in the
      mirror's load_mods.
- [x] **T8.5** Facade bindings: a package whose facade is re-export-only
      (no own `pub fn`) left the checker without the facade name
      ("unknown name 'pkg'"). LANDED in two pieces: consts ride the
      export table at load (pub-uses sell from rows, and a const had no
      row — statics stay module-private by law), and the qualified
      rewrites are const-aware (the dotted nullary `stdx.Ten` rides the
      same EX_MCALL shape as a call but lands in the consts table). The
      "re-exported type in the facade's own signature" half DISSOLVES:
      the spelling was illegal rho (bare struct literals), and the
      legal shape is boot's §8 ambiguity refusal — which the mirror
      MISSES (T8.14).
- [x] **T8.6** Interior→facade use cycle: a package file doing `use
      geom;` (legal per §2's two bases) was unresolved in the mirror —
      the module qualifier only ever tried the module's own dir.
      LANDED: the §2 second-base retry (own dir misses → the entry's
      dir) in both module-use resolution sites; witness rides
      run-mods-diff.
- [ ] **T8.7** Slice `==` refusal: boot refuses with "slices never
      compare (write a loop)" — the mirror ACCEPTS and compares only
      LENGTHS, silently (two same-length different-content slices
      compare equal). A checker-refusal gap, the worst class: the law
      is a compile error and the mirror hands back a wrong `true`.
- [ ] **T8.8** `std.json` through the runtime /mods face: the baked
      tree fails to load ("mods: unresolved module path(s)") while boot
      builds the identical shape green. Diagnosis landed as THREE
      faces: (1) the associated-call rewrite — FIXED, see T8.10's
      commit; (2) match arms over slice-payload variants — the MV
      binder width bug, T8.10; (3) whatever surfaces behind those two
      (the facade's parse fallbacks shrink with each fix — re-probe
      with the run-mods-diff harness patterns per fix).
- [ ] **T8.11** Closure captures of MANAGED handles miscompile in the
      mirror's emitter: `let mut xs: []i32 = make([]i32, 3);` captured
      by a closure reading `xs[0]` — boot prints 9, the mirror 0. The
      §6.8 hatch's emit path (the capture slot rides the wrong lane).
      Pre-existing; T8.9's checker work exposed it.
- [ ] **T8.12** BOOT-side (reference compiler): `let mut s: string`
      captured by a closure segfaults the COMPILED program (wasmtime
      rc 139; build succeeds). The managed hatch's string lane in
      boot's own emit — not a parity item, a reference defect the
      audit surfaced while picking a hatch witness.
- [ ] **T8.14** Cross-module type ambiguity refusal missing: a facade
      re-exporting a type it also names (via its own modules) is boot's
      §8 refusal ("ambiguous type 'Point' (visible from multiple
      modules)") — the mirror accepts and compiles. The dangerous
      direction: boot-red, mirror-green.
- [ ] **T8.13** The mirror's checker misses return-type mismatch:
      `fn() -> i32 { return len(s); }` (len is usize) — boot refuses,
      the mirror compiles. Part of the type-layer milestone.
- [ ] **T8.10** Slice-payload variant binder: `enum J { Arr([]i32) }`,
      `J.Arr([5, 6])` then `J.Arr(xs) => xs[0]` — boot prints 5; the
      mirror emits type-broken WAT (`i64.ge_u` fed an i32 lane — the
      pattern-bound slice's ptr/len lanes swap widths). Emit-level, in
      the match-binder path; the corpus-differential program rides the
      fix.
- [x] **T8.9** S6.8 mut-capture refusal: boot refuses a closure
      capturing a `mut` local ("a value copy would diverge; share a
      heap object through a pointer instead") — the mirror's checker
      accepts it and the runtime silently diverges (each call mutates a
      fresh copy; the outer binding never moves: `f() f() n` prints
      `2 2 1`). LANDED: the checker threads a value-type face per
      binding (annotation, or literal-init inference; untyped stays
      allowed — the law refuses only on knowledge) and the closure body
      walks its own scope stack (an inner `let` of the same name
      shadows the capture out; nested closures enforce their own;
      innermost binding wins). Managed handles stay legal per §6.8 —
      their emit is T8.11's bug.

---

## The design (the law — ratified 2026-09-25)

### 1. Identity

A small, hand-forged systems language; one binary toolchain;
reference-counted memory; zero undefined behavior; deterministic
compilation. The long-term direction is production-grade — this is
deliberately **not** written into public docs. The mainline is
boot → wasm → wasm self-hosting → gradual refinement; 0.1.0 is the wasm
self-hosting loop alone. MCU targets are a future backend, zero design
weight.

### 2. Memory

- RC + weak; every heap block carries a 24-byte header
  `{rc, wrc, drop}`; rc→0 runs the generated destructor; wrc>0 keeps the
  header (dead) so weak can observe death. No GC ever promised; no
  borrow checking ever promised — an evolution slot, not a commitment.
- **Zero UB is law on the safe subset**; violations are defined panics.
  The only unsafe window is the `intrinsics.` namespace.
- **Allocations are zeroed** (make/new read as 0; includes reused
  blocks).
- Counting is pure-local syntax insertion (one retain per copy of a
  managed value, one release per destruction); redundant pairs may be
  eliminated later; container ownership walks elements only at the
  rc==1 death check.

### 3. Types (the eleven rules)

1. Literals adapt to their consumer; with no consumer the default is
   **fixed forever**: integers `i32`, floats `f64`.
2. `const` infers from its initializer; an annotation may pin any
   builtin type.
3. The only conversion is `as` (integer wrap/truncate/extend; float→int
   truncates toward zero, saturates out of range; enum→tag). `as` has
   **one** semantics — constants truncate exactly like variables.
4. traits: `impl` blocks and bare methods coexist; multiple impl blocks;
   methods/impls may be defined in any module.
5. **Function overloading** with exact-match-unique resolution
   (parameter types + count + receiver); zero or many matches = compile
   error naming candidates. Trait satisfaction = name + signature match.
6. Method visibility is **import-scoped**: native methods travel with
   their type; extension methods participate only from modules in the
   caller's use closure; same-signature conflicts are a compile error
   naming both modules.
7. `use` has one form: dotted `use a.b.c;` (segments are identifiers).
8. Trait bounds (`[T: Show]`) exist and are verified per instantiation.
9. **Pointers are non-null by default**; nullability requires `?T`.
   `?T` is sugar for `Option[T]` — one absence mechanism; `weak.get()`
   returns `?*T`; no smart casts (unwrap via match/methods).
10. `==` comparability law: pointers identity, strings content, enums
    tag-then-payload, aggregates element-wise; `fn`/`dyn`/err-payloads
    never compare; comparing cyclic data ends in a stack-overflow panic
    (documented, not detected). On user types the law resolves through
    the **operator traits** (§11): the element-wise default holds until
    an `impl Eq for T` replaces it.
11. Everything is a value type; no moves, no borrows, no address-of.

### 4. Errors

panic = a programmer bug, and **process-fatal**: no catch, no recover,
ever; no unwinding machinery. `defer` runs on every scope exit path
except panic. `Result[T,E]`/`Option[T]` + `?` are the only error
channel.

### 5. Determinism

- Same compiler + same version + same target + same input →
  **byte-identical output** (diagnostics, symbol order, folds included).
- Different backends / different compilers: bytes differ, **behavior
  must match** (stdout + exit).
- Library containers have a **deterministic, documented** iteration
  order.

### 6. Declarations and visibility

`let` immutable by default (`mut` explicit; initializer required);
inner shadowing allowed, same-scope rebind is an error; `static mut` =
module-lifetime global (the only global state; sync laws arrive with
threads); visibility = pub/private + package facades; declaration order
free within a module; static initializers acyclic.

### 7. Build parameters

Root-file consts are ordinary consts (zero declaration restrictions).
`--set name=value` overrides any const whose type has a text form (bool,
all integer widths, f32/f64, string; range-checked; refusal = exit 2).
Comptime-known conditions fold: only the live branch is checked and
emitted; dead branches skip whole; reachability prunes modules. Root
consts are visible in every module (prelude status); shadowing them is
an error.

### 8. Strings

Immutable UTF-8 byte slices; len/index/slice are byte-based; no built-in
char abstraction (a utf-8 **package** may add code-point iteration).
`+` concatenates, never coerces. printf/eprintf/format: literal format
string, `{}` counted at compile time, every value prints via `to_str`;
aggregates are not printable. Triple-quoted strings are fully verbatim.
Hot string building uses `[]u8` buffers (the `__fmt_build` pattern).

### 9. Control flow

`if`/`match` are expressions (same-type arms; `if` requires `else`);
conditions are `bool`, no truthiness; aggregate values in let position
are **legal** (the old runtime panic is a bug to fix, not a feature).
**Labels, Go form**: `outer: while … { break outer; }` — plain
identifier + colon on `while`/`loop`; `break`/`continue LABEL`;
labels are function-unique and live in their own namespace. No goto.
TCO: direct self tail calls become loops; `return_call` is out.
Match is exhaustive unless `_` is present.

### 10. Modules and packages

One file, one module; two lookup bases (user's directory, then the
entry's); exactly one real body (`lib.rho` and `x.rho` coexisting is an
ambiguity error); package = directory behind a `lib.rho` facade, interior
files package-private, subpackages closed; `pub use` four forms;
`main` reserved in the root; use bindings are private; methods/impls
definable anywhere (the old ownership rule is gone — coherence is
enforced at call sites by exact-match-unique).

### 11. Operators

Integers wrap; `MIN / -1 = MIN`; `/0` `%0` panic; shifts mask by the
left width; floats are IEEE-754 (`/0.0` = inf, `NaN != NaN`); precedence
C-style, 11 levels + postfix, left-associative; `&&`/`||` short-circuit;
assignment is a statement with no value.

**Operator traits** (ratified 2026-09-26): `==`/`!=`/`<`/`<=`/`>`/`>=`
on user types resolve through the prelude traits `Eq` and `Ord` —
operator overloading with one shape, no magic:

- `trait Eq { fn eq(self, other: Self) -> bool }` — user structs/enums
  compare element-wise by default; an `impl Eq for T` **replaces** the
  default (never merges).
- `trait Ord { fn lt(self, other: Self) -> bool }` — explicit impl
  only; there is no lexicographic auto-derive. `a < b` calls `lt`; the
  other three derive: `a <= b` = `!(b < a)`, `a > b` = `b < a`,
  `a >= b` = `!(a < b)`. One method, one meaning.
  **Self LANDED (2026-09-27, step 1 of the construction order)**:
  boot lexes `Self` (K_SELF after K_STRING — the builtin range check
  in parse_type_inner stays intact), parse_type_inner yields the
  named type, trait sigs resolve Self as a type parameter of their
  own scope, impl members REWRITE Self to the impl's target before
  their signatures resolve (check.c's impl collection), and
  trait_satisfied unifies Self structurally (through * / slice /
  args — `*Self` vs `*Pt`). Self anywhere else is an unknown type.
  Two lang fixtures pin both directions; the suites and the corpus
  differential hold. THE OPERATOR TRAITS LANDED (2026-09-27): the prelude carries
      Eq/Ord/Hash (pointer-Self sigs — the boxed idiom; prelude.rho
      was reconstructed from the embedded truth first: the T3.6 wave
      had adapted prelude.c directly and left the .rho behind, and a
      blind re-embed regressed every mut view). ==/!= over user types
      REWRITE into the eq call when an impl exists (the full method
      machinery rides; a dangling-Node* across node_new corrupted the
      prelude's own check first — allocations move g_nodes), else the
      §10 defaults hold. Ordering over user types is impl-Ord-only:
      < calls lt, > is lt swapped, <= and >= the negations; the
      refusal names impl Ord. The default Hash is a synthesized FNV-1a
      fold over the == law's slots (op=7; slot-width bytes, i32 pair
      and pointer slots wrap to the i64 lane); impl Hash replaces it;
      the never-list refuses with its own message. Five fixtures pin
      eq-override/default/ord-derivations/ord-refusal/hash — suites
      518. THE REMAINING LEGS LANDED (same day): [T: Eq] dispatches
      statically (the instantiated == rewrites into the impl's eq —
      the bound's satisfaction already rode the method tables),
      dyn Eq .eq dispatches virtually, and BOTH ride two real
      pre-existing dyn fixes: the shim forwarded its arguments
      BEFORE the env-as-self (every dyn method with parameters
      called with crossed lanes), and the shim finder compared the
      trait's *Self param strictly (the method went unfound and the
      vtable slot pointed at nothing — self_unifies now exported to
      emit). The trait-scope ABI follows the dispatch type's lane,
      converting to the method's shape at the boundary. Suites 521;
      the cycle-through-user-eq overflow doc and the mirror side
      remain.
- `trait Hash { fn hash(self) -> u64 }` — the default folds the same
  values the `==` law compares, FNV-1a 64-bit over the slots in
  declaration order (string = content bytes; `*T` = the 32-bit
  address, little-endian; integers/floats = their little-endian bit
  patterns; bool = one byte; enums = tag then payload slots). An
  `impl Hash for T` replaces the default. Equal values hash equal —
  the defaults are built to; an impl overriding Eq but not Hash (or
  the reverse) is its author's to keep consistent.
- Builtins never consult the traits (direct emission, as today). The
  never-list — `fn` types, `dyn`, slices, `Result` — is **locked**:
  never comparable, never hashable, and no impl can unlock them; the
  `==` operator itself never applies to `dyn` (though a `dyn Eq` value
  dispatches `.eq` like any trait method).
- `Self` is a reserved word: inside a trait declaration it names the
  type satisfying the trait; inside an impl (and its methods) the
  impl's target type. Nowhere else.
- Bounds (`[T: Eq]`) are the first wave, ordinary §8 bounds verified
  per instantiation with static dispatch; virtual dispatch of the
  trait methods through `dyn` is ordinary §9 dyn dispatch.
- Coherence is §4/§5's law unchanged: impls in any module,
  exact-match-unique — two satisfying `eq` methods for one
  (trait, type) is the ordinary ambiguity error.

### 12. Closures and variadics

Closures capture locals by copy; capturing a **`mut` local of value
type** is rejected — two live paths to one mutable value could diverge
after a rebind. Capturing a **`mut` handle** binding (`*T`, `[]T`,
`string`, `dyn`) is legal: the copy is the shared view and the heap
object holds the state (§14's law governs stores through it) — the
escape hatch this rule's own rationale names (`new` a counter, pass
`*T`). Variadics: `rest: T...` last parameter, concrete element type,
`[]T` in the body, spread `xs...` last argument, call materializes a
fresh slice, variadic functions are not first-class values.

### 13. Toolchain architecture

boot = the complete design in C (the wasm backend only for 0.1.0), then
**frozen together with the language** — the design above is the ceiling;
growth from here is libraries only. The self-hosted compiler
(`libs/compiler`, one root, build parameters per §7) is written in rho
and graded behaviorally against boot. 0.1.0 ships **one backend:
wasm32-wasi**; native compilation joins afterwards as std-library
components (§ Phase 7), the language untouched. Trust root = **pure
source rebuild** (every gate run builds the seed from boot's C source);
the pinned `seed.wasm` remains as a byte-exact canary. Symbol policy:
lib artifacts keep public names and minify internals; cli/app artifacts
minify everything but `main`/`_start`; diagnostics never reference
mangled names; `-g` keeps full names. Optimizer backlog (ordered at
implementation time): operand-stack emission, string pooling, linear-scan
register allocation, escape analysis / rc-pair elimination. The
optimizer lives **in the compiler pipeline** — passes over the module
IR before serialization, rc-pairs first-class — never as an external
binary post-processor: binaryen cannot know the rc-pair law
(weak-observable death timing), so wasm-opt is not a long-term
dependency. It remains the site-asset shrinker (T5.1) until the
in-compiler optimizer demonstrably matches its effect, then retires.
The compiler grows the archive's hub back — strict SSA IR, lowered
once, serialized many ways (T3.8): WAT as the debug and interchange
contract pinned byte-identical through the refactor, the binary
serializer retiring wat2wasm ahead of the corpus close, a WAT→IR
decoder for the fixpoint leg, and the optimizer living in-compiler
as passes over the pinned IR (rc-pairs first-class). A minimal
in-boot interpreter (T3.13) executes the emitted surface in process,
its import surface bound to the kernel's by law, demoting wasmtime
to a short-term test-environment reference; boot stays wasm-only —
native emission is the self-hosted side's (Phase 7).

### 14. Kernel and std

The prelude holds exactly the mechanism-required kernel (Option/Result
+`?`, to_str + format sinks, panic hooks, allocator + rc glue, string
primitives, raw per-target syscall tails). **The kernel grows only when
a language mechanism grows.** `std` is the reserved in-repo directory;
collections (Vec/Map, deterministic documented order) and io (read_line,
file wrappers) are packages; the compiler consumes them; json is a
package; utf-8 is a future package. The bootstrap chain vendors in-tree
packages only. After 0.1.0, native compilation also lives in the std
library (Phase 7).

### 15. Sites and delivery

Two sites, one source: the repo's `site/` is the language home (GitHub
Pages); the bilingual course references the project, never duplicates
it. Playground: compile on the main thread, run in a worker (V8
worker-context miscompile — report upstream); caps per phase
(boot 120 s, compile 20 s post-download, run 10 s); fat functions put
vregs in linear memory; deploy asset = web config one generation past
the seed + wasm-opt. Editor code flows one way: repo → app.

### 16. Version policy

One version: **0.1.0**, tagged when the wasm self-hosting gate is green.
No other tags, no intermediate numbers, no evolution after the freeze —
the design above is the whole language. Native compilation is
post-0.1.0 std-library work, not a version of the language.

### 17. The test protocol

Two test forms, one verb. **File tests** — a file named `*_test.rho`
is a program: `fn main` runs, exit 0 passes, anything else fails;
`// out:` lines pin stdout byte-for-byte, `// exit:` pins the exit
code, `// set:` feeds build parameters (the corpus's own headers).
**Test blocks** — a top-level `test "name" { … }` is a synthesized
void fn in its own module: it sees everything the module sees
(white-box), judged by panic (assert) versus clean return. The verb
`rho test <path>… [filter]` takes files or directories (a directory
picks up `*_test.rho` files and files carrying test blocks, name
sorted); every test compiles as its own program instance — one test's
panic cannot fail another (the selection rides the emission side, the
program is rebuilt per test) — runs under a hard time cap, and the
verb exits 1 on any failure **including when nothing matched** (a
fake green is a red). Test blocks are checked in every mode (a broken
test is a compile error) and emitted only under the verb. No kernel
growth, no new mechanisms: the whole protocol lowers through §7's
build parameters and the existing panic law.

### 18. Mutability

`mut` is one keyword with one meaning, everywhere — it always sits
before the binding name (`let mut i`, `static mut LOG`,
`fn scale(mut p: *Rect)`), never inside a type, and means exactly:
**the view under this binding is writable.** A binding without `mut`
is a read-only view: stores through it (fields, slice elements),
calls of `mut`-receiver methods, and passing it where a `mut` view is
required are compile errors. `mut` is permission, never layout —
copying and the rc law (§2) are untouched; the view is shallow by
design: a handle copied out of any binding is governed by the new
binding's own `mut`.

Value parameters carry no `mut` (a copy has no view): they are
immutable, and the body rebinds with `let mut p = p;` when it must
mutate its copy. Handle parameters default to read-only and take
`mut` before the name to grant writes. The call site pairs with the
declaration: an argument passed to a `mut` parameter must be marked
`mut` at the call site, and an argument marked `mut` must land in a
`mut` parameter — both directions are compile errors — and the marker
requires the argument's own binding to be `mut` (you can only grant
what you have). Methods distinguish `fn Rect.area(self: *Rect)` from
`fn Rect.scale(mut self: *Rect, …)`; a non-`mut` binding cannot call
the latter, and a value can never call a pointer-receiver method.
Receiver `mut`-ness is not an overload axis and must match the
trait's signature exactly at impl time. Declaring `mut` without ever
writing through it is a hint (LSP), never an error. In impl methods
the receiver may be written bare — `self` or `mut self` — in both
homes: inside an impl block the type comes from the header
(`impl Show for Pt { fn to_str(self) -> string }`), in a dotted
method from the name (`fn Pt.to_str(self)`); read-only or writable
respectively, and the fully-typed form stays legal.

### 19. Match arm ergonomics

Three sugars over match arms, each lowering to the existing arm
rules. **Variant resolution by scrutinee**: an arm's variant name
resolves against the scrutinee's enum type first — `match o {
Some(v) => …, None => … }` needs no `Option.` prefix, for prelude
and user enums alike. The arm may only match the scrutinee's
variants (the type rule), so the resolution cannot be ambiguous;
there is no scope fallback — a variant of any other enum takes its
full path, in nested matches too (each match resolves by its own
scrutinee). **Or-patterns**: `pattern | pattern => arm` matches when
any alternative matches; every alternative must bind the identical
set of names, else a compile error. **Guards**: `pattern if cond =>
arm` — `cond` evaluates after the pattern matches, in scope of its
bindings; a guarded arm never counts toward exhaustiveness, so a
match whose arms are all guarded still requires a fallback.
