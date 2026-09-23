# todo

Living backlog. This file holds only what is still undone: the closed
rounds — the wasm bootstrap, the native wave, the optimizer, element-wise
`==`, tail calls, value containment, build parameters with the one
package root and the pinned seed, the dot-module round — are recorded
once, in git history and `docs/bootstrap.md`.

## NEXT — the full/native configuration bootstrap ring

Next round's first item, deferred by the owner's explicit call from the
build-parameters round (2026-09-24): the wasm and web rings close and
are graded every gate run; the FULL configuration's ring does not yet
close through a native-hosted compiler. The work:

1. **Native-target compilation of the compiler** —
   `build/gate/m.wasm` (or its native-built successor) compiles
   `libs/compiler/cli.rho` to all native targets (the crossings'
   `cross-*-self` legs already prove the builds; the ring goes further).
2. **Static structure verification at scale** — every self-image
   through `tools/image-check.py` (the gate's crossings already do this
   per run; the ring's images join the same law).
3. **Small-scale native-exec of the self-built compiler** — run a
   native self-built `rho` binary on a corpus slice and have IT rebuild
   the corpus to the goldens, under `tools/native-exec.sh`'s law
   (deliberately outside the gate — a corrupt image can wedge the
   kernel).
4. **Gate the ring** — a leg (or a sibling script the gate calls) that
   closes the full ring natively the way `--set native=false` closes the
   web ring, so "bootstrap complete" covers every configuration the
   compiler can produce.

## Open compiler bugs

### corpus 081 SIGSEGVs natively (arm64-mac)

`corpus/081_generics_bounds.rho` crashes (rc 139) natively — the only
red in `tools/native-exec.sh` (its `KNOWN` entry carries it; remove that
entry when it falls). Minimal repro: `printf(...)` followed by
`make([]string, 3)` (either alone passes; make-**first** also passes;
`[]i64` elements pass). Diagnosis as of 2026-09-23's deep round — much
narrowed, still open:

- The fault is inside an rc walker (`rel$s___string`'s element walk →
  `rc_dech`): a **stack** string struct's `hdr` field reads
  `0x100000000` — the image's LINK-TIME base, not a leaked adrp (a
  pc-relative adrp under a random slide cannot produce the bare link
  constant; there is no page-0 adrp and no baked 2^32 qword anywhere in
  `__DATA`).
- **Slide-dependent**: under a fixed base (lldb
  `target.disable-aslr true`) the same binary exits 0 and `rc_dech` is
  never even called with the bad value — the corruption's downstream
  path differs, so something upstream branches on an address value.
- The value is a link constant computed or carried at runtime; the
  per-run fault address is exactly `0x100000000` across different
  slides. Suspects: the fmt/varargs machinery's interaction with the
  free-list allocator handing a reused block to the later make, with
  some fn-pointer/addr materialization in that path encoding a link
  constant instead of a pc-relative address (the W-form encoder bugs of
  the previous round are the precedent).
- Ruled out: the tail-call pass (crashes identically with `--no-opt`),
  and uninit elements.
- Next steps for whoever picks it up: assemble `out.s` + the rt blob
  with the SYSTEM toolchain to get a symbol-rich twin (the in-tree
  image is stripped; `clang -arch arm64 -c` on `cat <blob> out.s` —
  the blob's `_rho_rt_heap` needs a `__HEAP` segment stand-in), then
  watchpoint the string struct that receives the bad hdr.

### Managed-make zeroing — attempted and REVERTED (2026-09-23)

The release walk may read every element of a `make([]T, n)`, and a
reused heap block hands back junk (the wasm allocator's zero-filled
fresh pages mask it — the same law as the layout-table zeroing). The
attempted `zero_bytes` emission in `lower_make` was correct on paper,
green on the corpus, and could not fix 081 — but the SECOND
self-hosting generation crashed: the child (even built `--no-opt`)
panicked with a null dereference in its own `dce_sweep` compiling the
mirror. Bisected precisely to that change. Spec §1.5 records the law
and the revert: whoever retries should emit the zero as a plain IR loop
(no cross-type casts, no prelude dependency) and re-run the FULL gate,
not just `--wasm`.

## Waiting on a Linux container (or matching hardware)

- amd64-linux / arm64-linux exec — the crossings stay build-only.
- In-container corpus — the corpus differential re-run inside a Linux
  container, per native target.

## Owner's call

- GitHub release — and the zips carrying self-built native clients.
- Bench the shipped compiler — the suite's rho lane builds with the
  boot seed; running it through the mirror (optimizer on) alongside is
  a methodology decision plus a full re-record.
- A LICENSE file at the repo root (`tools/lsp` and
  `tools/vite-plugin-rho` declare MIT).

## Later batches

- fmt-canonicalize `libs/compiler/` itself: the tree predates the
  mirror's own `fmt` (the blank-line convention around `use` blocks
  drifted); `m.wasm fmt -w` over the package is a mechanical round of
  its own — do not slip it into a feature round.

## Language backlog

- linear-scan register allocator (replaces spill-everything)
- escape analysis / rc-pair elimination (spec §3.4, §11.5)
- enum payload containment (the decl-time walk covers structs and
  arrays; variant payload types resolve through their own lane and get
  their pass when a real shape demands it)
- true cross-function tail calls (wasm `return_call`) — explicitly out
  of scope so far; the landed TCO is the loop form

## Standard library

Roadmap: `docs/stdlib-roadmap.md` — design, not implemented (Ring 1
`std/` does not exist; JSON is in flight; `read_line` rides the fs
item).
