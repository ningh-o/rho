# The rho standard library — roadmap

Where rho's standard library goes next: what exists today (with the
evidence), a placement law for what lands where, per-item *why now / why
not now* with dependency order, and the staged plan. Companion to
[language-service.md](language-service.md) (the compiler service API);
one section there depends on this roadmap and is called out below (§5).

Status: **design, not implemented** — except where noted as in flight.
Every claim is grounded in a file cited as `path:line` or in a probe
whose command and output are quoted.

## 1. What "stdlib" means here — the three rings

rho has no `std/` directory today. The closest things to a standard
library are, in order of coupling to the compiler:

- **Ring 0 — the prelude**, embedded in every compilation. A pure,
  target-independent **core** (`boot/prelude/core.rho`, 622 lines) plus a
  small per-target **tail** (`wasi.rho` 146 lines, `hosted.rho` 98,
  `mac.rho` 97). The core must stay byte-identical across targets; every
  target-specific fact lives in the tail
  (`spec/module-system.md:41-55`). Prelude items are visible everywhere
  without import (`spec/module-system.md:17-19`).
- **Ring 2 — packages**, today's only composition story: manifests,
  lockfiles, and in-tree `vendor/` consumed by plain module imports
  (`docs/package-manager.md`). No network registry; dependencies are
  vendored by reference.
- **Ring 1 — an in-tree `std/`** (this roadmap's destination): modules
  shipped with the toolchain, available to every program without
  vendoring. **Does not exist, and needs a module-system extension to
  resolve** — `use` paths are relative to the importing file and descend
  only (`docs/package-manager.md:42-46` probes `use ../x;` as a parse
  error), so a std root outside the program's tree is unreachable under
  the current resolution rule. Language/module-system evolution happens
  in the living toolchain first and is mirrored after
  (`docs/todo.md:123-159`, boot is the reference toolchain while the
  bootstrap gate is open).

**Placement law.** A capability goes in the first ring that fits:

1. The checker's desugars or builtins call it (`__fmt_print`/
   `__fmt_eprint`/`__fmt_build` — `spec/type-system.md:139-143`) →
   **prelude tail/core**, non-negotiable.
2. Teaching-level, target-independent, and small enough that "rho without
   it" is broken (`Option`, `Result`, `cat`, `panic`, the `to_str`
   family) → **prelude core**.
3. Everything else → **package first** (Ring 2, available *now*), then
   **promote to `std/`** (Ring 1) once the API has survived a round of
   real use. Promotion is cheap precisely because packages already
   compile as ordinary modules — promotion changes resolution, not code.

Why not put everything in the prelude? The reachability pass already
drops unused prelude machinery from artifacts
(`spec/spec.md:147-154`; `docs/frontend-guide.md:169-178` measures an
8.3 kB module that pulls only what it reaches), so *artifact size* is
not the reason. The reasons are: the prelude is embedded in the compiler
itself (growing it grows the compiler and its mirror set), prelude items
enter every module's unqualified scope (namespace pollution is a
language-level cost), and the cross-target byte-identity law makes the
core the most expensive place in the repo to iterate.

## 2. Inventory: what exists today

### 2.1 Prelude core (`boot/prelude/core.rho`)

`Show` (trait, line 21), `Option[T]` / `Result[T, E]` with `is_some`/
`is_none`/`is_ok`/`is_err` (lines 25-63), `panic`/`assert`/`assert_eq`
(65-80), the panic hooks `__panic_div`/`__panic_oob`/`__panic_null`
(82-92), string primitives `__streq`/`cat`/`__cat3`/`__substr`/`__rep0`
(94-149), the full `to_str` family on every primitive (151-232, plus the
`__Big` exact-decimal float machinery, 235-583), and the variadic format
sinks `__fmt_print`/`__fmt_eprint`/`__fmt_build` (585-611).

### 2.2 Prelude tails — the per-target hook set

Every tail implements the same hook names; this is the stdlib's real
portability contract:

| Hook | wasm tail | hosted/mac tail |
| --- | --- | --- |
| `__alloc`/`__free` | bump allocator over `HEAP` (`wasi.rho:19-28`) | `calloc`/`free` (`hosted.rho:19-27`) |
| `__print_str`/`__eprint_str`/`__exit` | `fd_write`/`proc_exit` (`wasi.rho:30-47`) | `write`/`exit` (`hosted.rho:30-41`) |
| `__open`/`__read`/`__write`/`__close` | over `path_open`/`fd_read`/`fd_write`/`fd_close`, fd 3 = preopen (`wasi.rho:71-110`) | libc externs (`hosted.rho:49-62`) |
| `__can_spawn`/`__program_args` | `false` / over `args_sizes_get`+`args_get` (`wasi.rho:112-146`) | libc-based (`hosted.rho:65-81`) |

### 2.3 Builtins and intrinsics

Compiler builtins recognized by name, unshadowable: `len`, `make`, `new`,
`panic`, `printf`, `eprintf`, `format`, `size_of`
(`spec/module-system.md:57-69`). The `intrinsics` namespace — the
minimal unsafe kernel: raw loads/stores by width, `memcpy`, `mem_set`,
`mem_move`, `f64_bits`/`f32_bits`, `slice_string`, `grow_pages`
(`spec/module-system.md:71-74`).

### 2.4 The capability envelope (measured, not remembered)

- **The emitted wasm import table is fixed at seven functions**:
  `fd_write`, `proc_exit`, `args_sizes_get`, `args_get`, `path_open`,
  `fd_read`, `fd_close` (`boot/src/emit_wasm.c:88-98`). No `fd_readdir`,
  no `path_create_directory`, no `sock_*`, no `clock_time_get`, no
  `random_get`. Unknown externs silently emit zero
  (`docs/package-manager.md:39-41`).
- **Native images are freestanding**: static ELF "no interpreter, no
  libc, raw syscalls" and Mach-O via dyld with the runtime blob
  (`spec/spec.md:16-17`, `docs/todo.md:71-95`). The RT blobs currently
  cover file IO + argv shims as a *mirror* item
  (`docs/todo.md:152-157`); networking syscalls are not among them.
- **`__program_args` works on wasm32-wasi with the working-tree
  compiler** — probe:

  ```sh
  $ rho/build/rho-boot build args.rho --target wasm32-wasi -o args.wasm
  $ wasmtime run --dir . args.wasm hello world
  argc=3
  wasi exit=0
  ```

  (argv[0] is the module name under wasmtime, hence 3.) The same call
  against the *tracked* artifact fails — `site/assets/rho.wasm`
  checking `self/rho.rho` reports `unknown function __program_args`
  (probe, `big.rho:13:10`): **the shipped artifact lags the working
  tree**, which is the normal state while the mirror gate is open. The
  native `run` arg passthrough has a rough edge —
  `rho-boot run args.rho -- hello world` answers `rho: cannot open
  world`, exit 2 (probe).
- **What rho programs demonstrably cannot do today** is documented from
  the `rho-pkg` build: no `fd_readdir`, no mkdir, no process spawn, no
  argv on old artifacts, and `use ../x;` unexpressible
  (`docs/package-manager.md:26-46`). That tool's stdin-only UI and
  hand-written TOML subset (`docs/package-manager.md:49-51, 83-88`) are
  the concrete price already being paid for the missing stdlib.
- **Spec drift to resolve**: `spec/module-system.md:51` says the tail
  defines "`read_line()` on hosted targets" — a grep over
  `boot/prelude/*.rho` finds no `read_line`. Either implement it or
  amend the spec; this roadmap assumes amend-or-implement lands with the
  fs item (§5.2).

### 2.5 The gaps, ranked by who feels them

1. No growable collections (`Vec`, `Map`) — every consumer hand-rolls
   them; the self-hosted compiler carries its own private `Vec`
   (`self/rho.rho:74`) as the existence proof.
2. No JSON — the package tool parses a hand-rolled TOML subset instead
   (§2.4); JSON is in flight in a parallel workstream this round (given
   by the task assignment; no rho JSON file exists in-tree yet —
   `find rho -iname '*json*'` shows only `bench/results.json` and
   node package files).
3. No path/args/fs wrappers over the hooks that already exist.
4. No time, no random (both blocked on the import table).
5. No sockets, therefore no http (blocked hardest; §5.3).

## 3. The evaluation criteria (why now vs why not now)

An item is **now** when most of these hold; **not now** when several
fail:

- **Pure computation?** Then it is testable in the corpus on every
  target today (`.rho`/`.out` pairs, `spec/spec.md:99-101`), needs no
  toolchain change, and adds no mirror debt. This is the strongest
  "now" signal there is.
- **Does it unblock a visible consumer?** The in-tree consumers are
  `rho-pkg`, the tests, and — per
  [language-service.md](language-service.md) §5.5a — the future `rho
  lsp` query loop, which is itself a rho program (line-framed JSON over
  stdio) and therefore needs JSON + args + fs-min and *no* http.
- **Toolchain change required?** Then the work is boot-first plus a
  mirror-set entry (`docs/todo.md:123-159`), and the bootstrap gate's
  open-ness means every such change compounds debt. Do these in
  batched rounds, only when the pure-rho value beneath them is
  exhausted.
- **Placement fits Ring 0?** (§1 placement law.) Most answers are no.

## 4. Dependency graph

```text
            path ──────────────┐
            │                  │
            ▼                  ▼
   Vec ──► Map          fs-min (open/read/write/close wrappers)
    │      │                  │            ▲
    │      │                  ▼            │ args (over __program_args)
    │      │            fs-full (readdir/mkdir)   │
    │      │                  │            ◄─── pkg tool v2, rho lsp loop
    ▼      ▼                  │
   JSON (in flight) ◄─────────┘ (file IO consumers)
    │
    ▼
   http-over-sockets ──► (needs Stage B/C socket hooks + TLS decision)
```

`time` and `random` hang off the same import-table round as `fs-full`
and feed `http` (dates, jitter) but nothing else in-tree.

## 5. The items

### 5.1 JSON — in flight (parallel workstream this round)

**Slot and dependencies, not design** (its design lives with its
implementation): a pure-computation module — parser + writer over
`string`/`[]u8` using `intrinsics.slice_string`, `cat`, and the
`format`/`to_str` round-trip law for numbers
(`spec/type-system.md:146-155` is the authority floats must round-trip
against). Land as a package (Ring 2), promote to `std/` in Stage D.

**Why now:** pure rho (zero toolchain change, corpus-testable
everywhere); retires `rho-pkg`'s TOML subset for any future
manifest/lock tooling; and the `rho lsp` loop of
[language-service.md](language-service.md) §5.5a is a natural consumer
(JSON request/response payloads). **Why not more:** no streaming, no
serde-style derive — rho has no reflection and no macros/codegen, so a
derive story is a *language* project, not a library one. Arbitrary-
precision numbers are out; i64/f64 coverage matches the language.

### 5.2 fs / path / args — the foundations. **Now, in this order.**

1. **`path`** — pure string computation (split/join/normalize/basename,
   downward-only semantics matching the module system's `use` law).
   **Why now:** zero toolchain risk, immediately consumed by fs and
   `rho-pkg` (which currently inlines path logic), corpus-testable on
   all four targets. **Dependencies:** none beyond `cat`/`__substr`.
2. **collections: `Vec`** — growable array over `make` + `intrinsics`
   (`memcpy`). **Why now:** every later item wants it; the self-hosted
   compiler's private `Vec` (`self/rho.rho:74`) is a large tested body to
   lift; pure rho. **`Map`** follows once string hashing (pure) is in —
   open addressing, tombstone-free; **why not before Vec**: nothing
   ships without Vec, while Map has no in-tree consumer yet.
   Traits note: 0.4.0's bounds (`[T: Show]`, `spec/type-system.md:169-191`)
   are enough to type a collection API; an `Iterator` protocol is
   *not* designed here — closures + `for`-less loops suffice, and a
   protocol decided before `Map` exists would be guessed.
3. **`args`** — thin wrapper over `__program_args()` returning
   `[]string`. **Why now:** the hook works on wasm with the current
   tree (§2.4 probe); the work is API polish, not capability. Flag the
   measured native `run` passthrough quirk (§2.4) as a toolchain bug to
   fix in the same round, not a blocker. **Dependencies:** none.
4. **`fs-min`** — `read_file`/`write_file`/`open`/`read`/`write`/
   `close` over the existing `__open`/`__read`/`__write`/`__close`
   hooks. **Why now:** all four hooks exist on both tails (§2.2) and
   the wasm import table already carries their syscalls
   (`emit_wasm.c:88-98`); `rho-pkg` is the paying consumer
   (`docs/package-manager.md` §2/§10). **Dependencies:** `path`.
5. **`fs-full` (readdir/mkdir) + `time` + `random`** — **not now, and
   blocked on one batched toolchain round**: all three need the fixed
   wasm import table extended (`fd_readdir`,
   `path_create_directory`, `clock_time_get`, `random_get` —
   `emit_wasm.c:88-98` grows) plus native RT-blob syscall mirrors
   (`docs/todo.md:152-157` covers file IO + argv only). **Why batch:**
   one emitter change + one mirror-set entry instead of three, and the
   bootstrap gate is already open (`docs/todo.md:123`). `read_line`
   (§2.4 drift) rides this same round — implement against
   `fd_read`, or strike the spec claim.

### 5.3 http / socket — **not now; the dual-track design, recorded so the delay is a decision.**

**Why not now, concretely:**

- Sockets need imports that do not exist in the emitted table
  (§2.4) — a toolchain round by definition.
- **WASI preview2 is the wrong first vehicle.** Preview2 means the
  component model: a componentized rho toolchain and a runtime that
  serves components. That is a strategic bet orthogonal to "one binary
  toolchain, no ceremony" (`spec/spec.md:20-22`) and to the in-tree
  emitter's fixed preview1 table. Preview1's socket calls
  (`sock_accept`/`sock_connect`/`sock_recv`/`sock_send`/`sock_close`)
  are supported by the runtimes rho already targets (wasmtime) and fit
  the existing emitter pattern — one table extension.
- **Native has no net syscalls yet** — the RT-blob work item covers
  file IO + argv (`docs/todo.md:152-157`); socket syscalls per target
  (arm64/amd64 linux, mac) are new blob work.

**The dual-track design for when it happens:** a small socket hook set
in both tails — `__sock_open`/`__sock_connect`/`__sock_accept`/
`__sock_read`/`__sock_write`/`__sock_close` — implemented **twice, behind
the same names**: (a) wasm track over preview1 `sock_*` (one
`wasi_imports[]` extension, mirroring §5.2's batch), (b) native track
over raw syscalls in the RT blobs, precedented by the existing
"no libc, raw syscalls" ELF story (`spec/spec.md:16-17`). Blocking IO
only — rho has no green threads and determinism argues against an
async runtime; a server is a loop with `accept`. **http client** is then
a *pure package* (request formatting, header parsing, chunked bodies)
— corpus-testable against a loopback fixture without any network.
**TLS is out of scope, deliberately**: hand-rolled crypto is a
liability, and the honest alternatives (C interop, an embedded TLS wasm)
are their own projects. http servers and preview2 both wait for the
client to prove the hook shape.

**The negative-space fact this roadmap pins: nothing above the editor
story needs http.** The language server speaks LSP over stdio bytes
(`tools/lsp/src/server.ts:55` — `createConnection(..., process.stdin,
process.stdout)`; `tools/lsp/README.md` "stdio transport"), and the
future `rho lsp` loop
([language-service.md](language-service.md) §5.5a) is line-framed JSON
over stdin/stdout in the same style. **LSP-over-stdio has no dependency
on http or sockets, in any stage of this roadmap.** Networking is for
rho *programs*, not for rho's tooling.

### 5.4 What is deliberately never stdlib (as of this writing)

- **TLS/crypto primitives** — §5.3.
- **Regex** — no in-tree consumer; JSON + `cat`/`__substr` cover the
  current need; a regex engine is a corpus-sized project that should
  wait for demand.
- **Async/green threads** — contradicts the determinism story; blocking
  hooks are the model (§5.3).
- **Reflection / serde derive** — no reflection in the language; derive
  is codegen the toolchain does not have (§5.1).
- **A registry client** — `docs/package-manager.md` §10 keeps
  registries out of the package tool for now; a network client does not
  change that decision by existing.

## 6. The staged plan

| Stage | Contents | Toolchain change? | Gate |
| --- | --- | --- | --- |
| **A — pure rho, now** | `path`; `Vec` (then `Map`); JSON (in flight); `args`; `fs-min` | none | corpus pairs for every module on all four targets (`rho test`, `spec/spec.md:99-101`); delivered as vendored packages, promoted later |
| **B — one batched toolchain round** | import-table extension (`fd_readdir`, `path_create_directory`, `clock_time_get`, `random_get`); native RT-blob mirrors; native `run` args fix; `read_line` implement-or-amend | yes — boot + mirror set (`docs/todo.md`) | twice-compile determinism gate; `boot(corpus) == self(corpus)` progress not regressed; then `fs-full`/`time`/`random` modules land on the new hooks |
| **C — sockets dual-track** | `__sock_*` hooks both tracks (preview1 `sock_*` / raw syscalls); `http` client package; loopback corpus fixture | yes — same shape as B | client works against loopback on wasm **and** native before any server work; TLS decision explicitly re-visited, default remains "out" |
| **D — promotion to Ring 1** | `use std/...` resolution rule (module-system extension in the living toolchain); `std/` tree absorbing the Stage A modules | module-system change | stable API after a round of real use; resolution rule specified in `spec/module-system.md` with corpus + pkg-fixture coverage (`tests/pkg-fixture/run.sh` is the model, `docs/package-manager.md:300`) |

Sequencing logic, in one line each: Stage A is pure value at zero
mirror cost; Stage B batches everything that touches the emitter;
Stage C reuses B's batch pattern for the hardest capability; Stage D is
bookkeeping once the APIs have users.

## 7. Testing law

- Every pure module ships with corpus `.rho`/`.out` pairs run on all
  four targets (`rho test`, `spec/spec.md:99-101`) — same law as the
  language itself.
- Every hook-backed module gets a dual-target test: wasm under wasmtime
  (with `--dir .` where files are involved) and native where the blob
  supports the syscall (§2.4's probes are the pattern).
- Anything crossing Stage B/C's toolchain changes also enters the
  mirror-set checklist (`docs/todo.md`), or it is not done.
- End-to-end capability proofs live as fixture scripts, the way
  `tests/pkg-fixture/run.sh` proves the package tool end to end
  (`docs/package-manager.md:292-300`).
