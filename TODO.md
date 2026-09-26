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
- [ ] **T2.2+** Port module by module; each module its own TODO; graded
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
      T_LBRACE form), the match statement eats its trailing `;`, and
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
- [ ] **T2.x** Optimizer in the mirror: constant folding, dead-code
      elimination, globals tree-shaking, tail-call→loop — with the
      language suites (opt/eq/params/modsys/strops/multiline) rebuilt
      for the new language. wasm emit only — **no native backends in
      0.1.0.** The wave lands as one consolidation pass: after §17–§19
      and T3.5–T3.8 are in, boot and the mirror get a refinement sweep
      (structure, naming, dedup of the growths' accumulated patterns)
      before the suites pin them — features first, polish second,
      never interleaved.

## Phase 3 — gates and the trust root

- [x] **T3.0** GitHub CI: one workflow on push + PR — clang builds
      boot, `make test` runs as the leg (every script already
      time-capped); wabt and wasmtime pinned by version, never
      floating. When T3.1's gate.sh lands it becomes the leg list.
      The workflow is dormant until master pushes resume (push timing
      stays the owner's call). The native ring's ephemeral-runner CI
      (T7.x) rides this same workflow when it exists.
- [ ] **T3.1** gate.sh rebuilt: boot selftest; corpus differential
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
      its parent directory. The mirror now runs its whole pipeline:
      it loads lex, parse (75 fns), and check clean, then crashes
      parsing emit.rho's own text — verbatim strings, static muts,
      comments, and the fixed call-arg caps all ruled out; the crash
      follows a shower of recovered statement fallbacks. Next tool:
      a top-level fn-span stubber (fn header lines to their col-0
      closing brace — the first attempt miscounted spans; verify the
      span count against `grep -c '^fn \|^pub fn '` before trusting
      it) to bisect emit.rho's 60 fns half by half.
- [ ] **T3.2 Pure-source trust root**: every gate run rebuilds the seed
      from boot's C source on the spot. The pinned `seed.wasm` stays in
      the repo **as a canary**: rebuild, compare byte-for-byte (D1 makes
      this exact), inequality = determinism alarm.
- [ ] **T3.3** Differential fuzzing rebuilt for one implementation:
      opt-on vs opt-off self-differential + golden corpus replay. (The
      old boot-vs-mirror differential retired with the old code — the
      fuzz framework is the price of the clean slate; rebuild it before
      calling anything done.)
- [ ] **T3.5** The test protocol (§17) — **lands before T3.4, so the
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
      leg: rho's test suite running rho).
- [ ] **T3.4** Suites: lang/modsys/opt/eq/params/multiline/strops/diag
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
      Two tiers: green files gate; a case for law already ratified
      but not yet implemented (§18 mut view, §19 match ergonomics,
      T3.7 item imports, the T3.6 usize lane) carries
      `// pending: T3.x` and runs as an expected-fail ledger,
      promoting per case when its task lands. The suite runs on
      T3.5's verb — it is the verb's fixtures and acceptance; no
      interim runner exists. The existing tests/check
      negatives stay put — no migration churn.
- [ ] **T3.6** The mut view law (§18) + the usize lane. Boot surface
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
- [ ] **T3.7** Module item imports (module-system.md §2/§4/§6,
      syntax.md §4.4): the final use-segment binds a public item
      unqualified (`use lex.a as b;`), the brace form expands one
      plain use per item, module-vs-item and name collisions are
      errors naming both, and item imports stop at package facades.
      Implemented with the T3.4 modsys suite; the self-host mirrors.
- [ ] **T3.8** The in-tree wasm toolchain, sequenced after the corpus
      differential closes (108/108) so the mirror's climb is not
      disturbed mid-growth: `std.wasm` — an encoder/decoder package
      written in rho — plus boot's C counterpart, pinned
      byte-identical against each other; the compiler pipeline grows
      a module IR with two serializers (WAT text stays the debug and
      interchange contract; the binary path retires wat2wasm).
      Acceptance: dual-run byte-compare over the whole corpus, the
      wat→wasm→wat fixpoint leg (fmt's law, applied to assembly), and
      a readable decode diff for the T3.2 canary.
- [ ] **T3.9** Match arm ergonomics (§19): variant resolution by
      scrutinee (bare `Some`/`None`/`Ok`/`Err` and user-enum variants,
      full paths stay legal, no scope fallback), or-patterns with the
      identical-binder law, guards with the never-exhaustive rule.
      Acceptance legs: bare prelude-variant and user-variant arms
      (nested inner match resolves by its own scrutinee); full-path
      cross-enum still legal; or-pattern union binder + binder-set
      mismatch rejection; guard selects conditionally, guard
      referencing bindings, guarded-only match demanding a fallback;
      fmt round-trips all three forms. Boot implements; the self-host
      mirrors.
- [ ] **T3.10** Bare receiver in impl methods (§18): `self` = `*T`
      read-only, `mut self` = writable — the type inferred from the
      implemented type; fully-typed receivers stay legal; signature
      match (impl vs trait) keeps the receiver mut form. Acceptance:
      bare-self impl satisfies a trait, bare `mut self` writes through
      with the §18 gates, typed and bare forms mix in one impl, fmt
      round-trips.

## Phase 4 — kernel boundary and the std library

- [ ] **T4.1** Kernel audit: the prelude contains exactly the
      mechanism-required set (Option/Result+`?`, to_str + format sinks,
      panic hooks, allocator + rc glue, string primitives, raw per-target
      syscall tails). Nothing else. The standing law: **the kernel grows
      only when a language mechanism grows.**
- [ ] **T4.2** `std` = the reserved in-repo directory; `use std.io;`
      resolves by the single rule (first segment `std` → the reserved
      directory); every other `use` stays two-base relative.
- [ ] **T4.3** std.collections: Vec and Map as real packages (extracted,
      not copied, from the compiler's own source; the compiler consumes
      them afterwards). Map iteration order is **deterministic and
      documented** (D3).
- [ ] **T4.4** std.io: read_line, file read/write wrappers over the raw
      tails.
- [ ] **T4.5** json as a package on the new language; rho-pkg updated
      (manifests, lock, vendored path+git deps).
- [ ] **T4.6** (library, non-blocking) utf-8 package: code-point
      iteration and friends — a package, never the kernel.

## Phase 5 — sites and course

- [ ] **T5.1** Language home (site/): hero, tour, playground, spec
      reader — rebuilt around the new compiler; playground = compile on
      the main thread, execute in a worker (V8 worker-context miscompile
      still unreported — minimize and report upstream); phase-split caps
      120/20/10 s; fat functions to linear memory (`w_memmode`); deploy
      asset = one generation past the seed + `wasm-opt -Oz
      --enable-bulk-memory` — wasm-opt stays the site-asset shrinker
      until the in-compiler optimizer (§13) demonstrably matches its
      effect, then retires.
- [ ] **T5.2** Course (the bilingual app): all live blocks re-pinned to
      the new language; the honest-limitation notes rewrite (aggregate
      let-position now legal; `?T` non-null taught as the one true
      absence form); editor stays single-source from the repo.
- [ ] **T5.3** (owner's call, do not self-deploy) add the deploy
      workflow and deploy the course site.

This phase's ecosystem delivery — the vite plugin and the prettier
plugin — is specified in [docs/ecosystem.md](docs/ecosystem.md): goal,
dependencies, shape, acceptance law, npm version policy. They start
only when their listed dependencies close.

## Phase 6 — freeze and 0.1.0

- [ ] **T6.1** Full gate green: every leg, corpus differential, suites,
      fuzz, both sites building — all on the wasm self-hosting loop.
- [ ] **T6.2** Tag `v0.1.0` — the one and only version. Release zip:
      `rho-0.1.0-wasm32-wasi.zip`, binary named `rho.wasm`, SHA256SUMS,
      English RELEASE.md. Push/tag/deploy timing belongs to the owner.
- [ ] **T6.3** **Freeze.** boot and the language freeze together. From
      here the language grows no more; 0.1.0-era growth is libraries
      (utf-8, collections, io, net) and tooling quality (operand-stack
      emission, string pooling, linear-scan register allocation, escape
      analysis / rc-pair elimination — ordering decided when the freeze
      lands; the §13 in-compiler optimizer's finish line includes
      retiring binaryen from the site pipeline once its effect is
      matched).
- [ ] **T6.4** Corpus dissolution (after T6.3). corpus's historical
      role — behavioral memory of the pre-rewrite language — expires at
      the freeze; spec + suites own truth from there. Retire the
      `corpus/` directory by the three-way split: cases the suites
      already cover die; good teaching programs promote to `examples/`
      (user-facing, still gated so they cannot rot); whole-program
      interaction pins move to the suites' programs tier and the
      differential re-points there. What never retires: the
      whole-program integration layer, the differential base, byte
      goldens, the examples — only corpus's unanchored positives-only
      form retires.

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
    (documented, not detected).
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

### 12. Closures and variadics

Closures capture immutables by copy; **mut capture stays rejected**
(shared mutable state is a heap object: `new` a counter, pass `*T`).
Variadics: `rest: T...` last parameter, concrete element type, `[]T` in
the body, spread `xs...` last argument, call materializes a fresh slice,
variadic functions are not first-class values.

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
The in-tree assembler/decoder (`std.wasm` + boot's C counterpart,
byte-pinned against each other) retires wat2wasm once the corpus
differential closes (T3.8).

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
