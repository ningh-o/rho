#!/bin/zsh
# the corpus differential (T3.1's second leg): every corpus program
# through boot AND through the self-hosted chain; behavior (stdout +
# exit) must match. The pass count is pinned — it may only grow; the
# closing of this leg IS the corpus differential going green.
#
# Every file that fails its first pass re-runs ONCE (bounded): a
# hundred wasmtime spawns per run sit atop macOS's memory-pressure
# cliff, and an environment-blip (a short read under load, a busy
# rename) must never paint a green leg red. A REAL regression fails
# both times; the pin keeps its teeth.
set -u
RHO=${RHO:-./build/rho}
# The consolidation merge (2026-09-26) dipped the floor 108 → 98: the
# corpus now speaks the mut law and the u32 usize lane, and the
# self-host parsed neither. Both surfaces landed the same night —
# the mirror parses `mut self`/param prefixes and call-site markers
# (permission only, never layout) and rides usize at the wasm32
# address width with the declared-width shift-count mask — closing
# all ten legs (086/088/092/098/105 mut surface; 005/041/045/046/048
# the lane and the mask). 108 again, on the merged law.
#
# 108 → 110 at the T3.1 frontier round: two new legs pin the
# self-compile fixes — 106 (a ten-parameter signature; the mirror's
# fixed cap of eight indexed past its param tables and collapsed the
# whole parse) and 107 (a struct and enum born inside a package; the
# module merge never registered them, so module-local `new` stayed
# unknown to the self-host's own checker).
#
# 110 → 111 with n14_io_stdin: the __ face (std.io's raw wasi window)
# now rides the self-host twin too — imports, the four wrappers, and
# __string_from as a build_string_pair face; the differential grades
# both compilers on stdin/window/write behavior.
#
# 111 → 112 with n15_weak_reuse: the weak probe observes death, not
# reuse — the node dies, a same-size churn runs, and weak.get() still
# reads None (§1.1's wrc-keeps-headers law, graded on both compilers;
# the case goes red the day either allocator starts recycling blocks
# without honoring the weak header).
#
# 112 → 113 with n16_collections: std.collections rides the
# differential — the loader bakes the std/ tree into MODS, and the
# program pins the Vec and Map surfaces through the clones (push/
# get/contains/remove/pop, the ?V option matches, bool words, D3's
# ascending-key iteration). Four emitter gaps fell out: the ST_LET
# slice-call gate and enum arm now read method returns through the
# instantiation (an EX_MCALL init bound scalar once — len(view) fell
# through to an undefined $u_len and a ?V let's match matched
# nothing), build_string_pair's method arm resolves the declared
# return via method_rtype and targets the per-instantiation clone,
# ginst_method_target picks the overload whose receiver instantiation
# matches the call, and method_rtype sees bare-registered methods.
#
# 113 → 114 with n17_const_flood: parse()'s accumulation tables ride
# std Vec (wave 3) — 70 chained consts plus string/bool/static mut
# ride the tables past the old fixed cap of 64, and an 18-variant
# enum (with a two-payload variant at index 16) rides vnames/vslots
# past the old fixed 16 and the payload pool past its fixed 128
# slots. Boot strictness pinned in passing: main must return i32,
# statics are always `static mut`, and the last enum variant takes no
# trailing semicolon.
#
# 114 → 115 with n18_table_grow: the declaration tables grow — 33
# structs, 17 traits, 33 enums ride parse()'s tables past the old
# fixed caps (32/16/32; the fn table already grew, and the const and
# enum-variant tables moved to std Vec the wave before). The same
# sweep lifted the silent caps: the use tables (a dropped 33rd use
# was a miscompile by omission), the module-canon table (a skipped
# registration reloaded the module), and the export tables (a refused
# row left the name unknown to the checker).
#
# 115 → 116 with n19_targs: THE WALL LIFTS — call-site type
# arguments. name[T1, T2](args) names a generic at the call site (a
# factory's T rides only its return, so argument inference can never
# bind it); boot gained the grammar (parse lookahead to ']' + '(' on
# both the plain and the dotted call), the seeded instantiation, and
# the deep substitution that maps make([]T)/new Vec[T] inside an
# instance body (the bare-param check missed composite spellings).
# std grows the generic factory vec_of[T], and the twelve concrete
# push/insert overloads collapse into one generic form each (the
# make-substitution law they were waiting for). The case pins the
# compiler-consumption shape: vec_of[*Pt] instantiates over a type
# the PROGRAM owns, and the generic push serves it.
#
# 116 → 117 with n20_pair_shapes: the pair-lane shapes the site's
# runtime face exercised — a string fn returning a match (the arms
# ride the pair lanes through the return), a string param fed from
# an Option binder (the payload text rides the call), and a value
# enum's to_str concat (a to_str method call is stringy by law).
# The std-era full spellings — Option[string] / Result[T, E] as
# declared return types — now carry their payload text through
# eat_type, and optpt_of_scrutinee reads it off the first bracket
# arg.
#
# 117 → 118 with n21_absence_faces: the absence faces over the
# builtin boxes (is_ok/is_err/is_some/is_none — the constructors'
# hardcoded tags decide), the bool match (true/false are literal
# patterns, not wildcards), and the printf hole lanes for method
# results (f64 rides method_rtype).
#
# 118 → 122 with the fuzz campaign's first four finds (T3.17's
# discovery leg, tools/fuzz/gen.mjs, seeds 1-150): n22 — f32
# arithmetic rounds every OPERAND to f32 (the compound assign once ran
# in f64 and demoted only the result); n23 — a comparison's value is
# bool whatever its operands' width (a bool let fed from a float
# compare once bound a float lane and printed 0.0); n24 — a divish op's
# dividend parks in a fresh local (the shared $r scratch died to a
# nested divish on the rhs: (a^b)/(c%38) read the rhs's lhs as the
# dividend); n25 — the constant fold wraps AND sign-extends at the
# operand width like the runtime (the if-condition fold skipped the
# i8 wrap: (-14)*(-32) folded 448 while the runtime read -64, and a
# zero-extend-only wrap read the i8 -64 as 192). Each case names its
# seed in the header — the file outlives the bug as a regression.
#
# The floor stays 122 through the assignment-law wave: the mirror's
# checker grew the §18 mut law (let-without-mut, non-mut params, match
# binders, and consts refuse their writes with boot's exact
# diagnostics; fmt stays syntax-level and checks nothing), which turns
# refusals the corpus never carried — every pinned case already spoke
# the law, so all 122 hold.
#
# 122 → 94 at the corpus dissolution (T6.4): corpus/ retires and the
# whole-program pins re-home as tests/suites/programs/ — the textual
# goldens re-anchored as // out: headers judged by the suite verb, one
# byte golden (n11) riding its .out beside the runner that judges it,
# fourteen teaching programs promoted to examples/ (gated by
# tools/verify-examples.mjs), and fourteen smoke shapes retired under
# confirmed suite holders (divrem_trunc, int_wrap, labels,
# defer_exit_paths, bool_condition, if_match_expr — and printf itself,
# the body of every suite case). The differential's base is the
# programs tier now; the pin is its size.
#
# 94 → 95 with the statement-match spelling closure (T3.18's
# residue): the mirror read a trailing ';' after a statement match as
# optional — boot's statement match shares the expression form's
# parser and refuses it (the suite pin
# stmt_match_trailing_semi_expect_test holds the refusal; the mirror's
# parse now refuses with boot's wording). t13 pins the legal
# neighborhood the closure guards — statement match mid-function,
# inside control flow, nested in a block arm — so the surface stays
# graded on both compilers.
PINNED=95

# one file's differential; echoes "pass" or the failure label
check_one() {
  local f=$1
  local name src sets s kn vv mods mf
  name=$(basename "$f" .rho)
  sets=()
  src=$(cat "$f")
  # the module tree rides MODS for the self-host side (boot reads the
  # same tree from disk): every rho file under the corpus's packages,
  # plus the reserved std/ tree (paths stay repo-relative — the
  # loader's std resolution looks up "std/..." in the baked tree)
  mods=""
  for mf in tests/suites/programs/geom/*.rho(N) tests/suites/programs/geom/*/*.rho(N) tests/suites/programs/web/*.rho(N) tests/suites/programs/pk/*.rho(N) tests/suites/programs/pk/*/*.rho(N) tests/suites/programs/pk/*/*/*.rho(N); do
    if [ -f "$mf" ]; then
      mods="$mods@MOD@ ${mf#tests/suites/programs/}
$(cat "$mf")
"
    fi
  done
  for mf in std/*.rho(N) std/*/*.rho(N) std/*/*/*.rho(N); do
    if [ -f "$mf" ]; then
      mods="$mods@MOD@ ${mf}
$(cat "$mf")
"
    fi
  done
  for s in $(sed -n 's/^\/\/ set: //p' "$f"); do
    sets+=(--set "$s")
    kn=${s%%=*}
    vv=${s#*=}
    src=$(printf '%s\n' "$src" | sed -E \
      "s/^(const +${kn} *: *[A-Za-z0-9?*]+ *= *).*/\1${vv};/")
  done
  bgot=$("$RHO" run "$f" 2>/dev/null ${sets:+${sets[@]}}); brc=$?
  if ! "$RHO" build libs/compiler/main.rho -o /tmp/cdiff-c-$$.wasm \
      --set "SRC=$src" --set "MODS=$mods" >/dev/null 2>&1; then
    echo "build"; return
  fi
  wasmtime /tmp/cdiff-c-$$.wasm >/tmp/cdiff-$$.wat 2>/dev/null
  local cr=$?
  rm -f /tmp/cdiff-c-$$.wasm
  if [ $cr -eq 1 ]; then
    # a clean refusal (diagnostics on stderr, exit 1) — a missing
    # feature, not a wrong behavior; falling through would compare
    # boot's output against an EMPTY wasm and mislabel it :diff
    echo "refuse"; return
  fi
  if [ $cr -gt 1 ]; then
    # the compiler itself died mid-run — worse than any behavioral
    # diff; the robustness bar is clean refusal or clean compile
    echo "PANIC"; return
  fi
  if ! wat2wasm /tmp/cdiff-$$.wat -o /tmp/cdiff-$$.self.wasm 2>/dev/null; then
    echo "w2w"; return
  fi
  rm -f /tmp/cdiff-$$.wat
  local sgot src_rc
  sgot=$(perl -e 'alarm 10; exec @ARGV' -- wasmtime \
    /tmp/cdiff-$$.self.wasm 2>/dev/null); src_rc=$?
  rm -f /tmp/cdiff-$$.self.wasm
  if [ "$brc" -eq "$src_rc" ] && [ "$bgot" = "$sgot" ]; then
    echo "pass"; return
  fi
  echo "diff"; return
}

pass=0; fail=0; failed=""
for f in tests/suites/programs/*.rho; do
  name=$(basename "$f" .rho)
  r=$(check_one "$f")
  if [ "$r" != "pass" ]; then
    # the bounded re-check: environment blips fail once, regressions
    # fail twice
    r=$(check_one "$f")
  fi
  if [ "$r" = "pass" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); failed="$failed $name:$r"
  fi
done
echo "corpus differential: $pass pass, $fail fail (pinned floor: $PINNED)"
if [ "$pass" -lt "$PINNED" ]; then
  echo "REGRESSION below the pinned floor"
  echo "failed:$failed"
  exit 1
fi
if [ "$fail" -gt 0 ]; then
  # the remaining surface, listed but not fatal — the leg closes when
  # this list empties
  echo "remaining:$failed"
fi
