#!/bin/sh
# tests/lang/params — the build-parameter suite (spec §7.2/§7.3): the root
# file's consts ARE build parameters, `--set` overrides them by name, and a
# comptime-known if condition keeps only its live branch.
#
#   first line `// exit: N` — build (with the `.set` file's pairs when
#                             present), run under wasmtime; stdout must
#                             equal <name>.out and the exit code N. A
#                             <name>.default.out builds the same source
#                             with NO overrides and grades that golden —
#                             one source, two configs, both laws pinned.
#   first line `// diag`    — `check` diagnostics must equal main.out
#   first line `// refuse: N` — the build must exit N with stderr equal to
#                             <name>.out (the CLI-level refusals)
#   <name>.irno             — grep -E patterns that must NOT appear anywhere
#                             in the --dump-ir output (dead modules shaken)
#   <name>.irhas            — grep -E patterns that MUST appear in the dump
#   <name>.flags            — extra build flags, one per line (e.g. `-g`)
#
# Mirror-only by design: the folding and the params live in the
# self-hosted compiler (boot is frozen). The mirror lane follows the
# modsys runner: the pinned seed builds the merged root (cli.rho) when no
# fresh artifact exists.
#
# Usage: sh tests/lang/params/run.sh
set -u
cd "$(dirname "$0")/../../.."

M=${MIRROR:-build/gate/params-mirror.wasm}

wr() {
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' "$@"
}

wt() {
  wr "$1" wasmtime run --dir . "${@:2}"
}

strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

# the mirror lane: rebuild whenever any mirror source is newer. Since the
# dot round the mirror's own sources speak the pinned seed's language, so
# the seed is the builder (docs/bootstrap.md, the re-pinning ritual).
ROOT=libs/compiler/cli.rho
if [ ! -f "$M" ] || [ -n "$(find libs/compiler -name '*.rho' -newer "$M" -print -quit 2>/dev/null)" ]; then
  rm -f "$M"
  if ! wt 900 boot/rho-seed.wasm build "$ROOT" --target wasm32-wasi -o "$M" >/dev/null 2>&1 || [ ! -f "$M" ]; then
    echo "params: the pinned seed cannot build the mirror"
    exit 2
  fi
fi

G=build/gate/params
mkdir -p "$G"

# the .set file's lines become `--set name=value` arguments
set_args() {
  SETS=""
  setf="$1"
  [ -f "$setf" ] || return 0
  while IFS= read -r pair; do
    [ -z "$pair" ] && continue
    SETS="$SETS --set $pair"
  done < "$setf"
  return 0
}

# a .flags file's lines are passed through verbatim (e.g. `-g` — full
# internal symbol names; the default artifacts use the shortest serial
# names, which f14 pins)
flags_args() {
  FLAGS=""
  flagsf="$1"
  [ -f "$flagsf" ] || return 0
  while IFS= read -r fl; do
    [ -z "$fl" ] && continue
    FLAGS="$FLAGS $fl"
  done < "$flagsf"
  return 0
}

fails=0
total=0
# fixtures: single files and directories (directory form: main.rho + friends)
for tgt in tests/lang/params/*.rho tests/lang/params/*/; do
  case "$tgt" in
    */) dir_fix=1; dir=${tgt%/}; name=$(basename "$dir"); src="$dir/main.rho" ;;
    *)  dir_fix=0; dir=""; name=$(basename "$tgt" .rho); src="$tgt" ;;
  esac
  [ -f "$src" ] || continue
  total=$((total + 1))
  marker=$(head -1 "$src")
  base="$G/$name"
  if [ "$dir_fix" = 1 ]; then out="$dir/main.out"; else out="tests/lang/params/$name.out"; fi
  SETS=""
  FLAGS=""
  if [ "$dir_fix" = 1 ]; then set_args "$dir/main.set"; else set_args "tests/lang/params/$name.set"; fi
  if [ "$dir_fix" = 1 ]; then flags_args "$dir/main.flags"; else flags_args "tests/lang/params/$name.flags"; fi
  # shellcheck disable=SC2086
  eval "set -- $SETS"
  verdict=""

  case "$marker" in
  "// exit:"*)
    want_exit=$(printf '%s' "$marker" | sed 's|// exit: ||')
    rm -f "$base.wasm"
    # shellcheck disable=SC2086
    if ! wt 90 "$M" build "$src" --target wasm32-wasi -o "$base.wasm" "$@" $FLAGS >/dev/null 2>&1 || [ ! -f "$base.wasm" ]; then
      verdict="FAIL (build)"
    else
      wt 10 "$base.wasm" 2>/dev/null | strip_dbg > "$base.got"
      wt 10 "$base.wasm" >/dev/null 2>&1
      got_rc=$?
      if ! cmp -s "$base.got" "$out"; then
        verdict="FAIL (output)"
      fi
      if [ -z "$verdict" ] && [ "$got_rc" != "$want_exit" ]; then
        verdict="FAIL (exit $got_rc, want $want_exit)"
      fi
    fi
    # the default build (no overrides), when a golden pins it
    if [ -z "$verdict" ]; then
      if [ "$dir_fix" = 1 ] && [ -f "$dir/main.default.out" ]; then
        defout="$dir/main.default.out"
      elif [ -f "tests/lang/params/$name.default.out" ]; then
        defout="tests/lang/params/$name.default.out"
      else
        defout=""
      fi
      if [ -n "$defout" ] && [ -n "$SETS" ]; then
        rm -f "$base.def.wasm"
        if ! wt 90 "$M" build "$src" --target wasm32-wasi -o "$base.def.wasm" >/dev/null 2>&1 || [ ! -f "$base.def.wasm" ]; then
          verdict="FAIL (default build)"
        else
          wt 10 "$base.def.wasm" 2>/dev/null | strip_dbg > "$base.def.got"
          wt 10 "$base.def.wasm" >/dev/null 2>&1
          def_rc=$?
          if ! cmp -s "$base.def.got" "$defout"; then
            verdict="FAIL (default output)"
          fi
          if [ -z "$verdict" ] && [ "$def_rc" != "$want_exit" ]; then
            verdict="FAIL (default exit $def_rc, want $want_exit)"
          fi
        fi
      fi
    fi
    ;;
  "// diag")
    wt 120 "$M" check "$src" 2>&1 >/dev/null | strip_dbg > "$base.got"
    if ! cmp -s "$base.got" "$out"; then
      verdict="FAIL (diag)"
    fi
    ;;
  "// refuse:"*)
    want_rc=$(printf '%s' "$marker" | sed 's|// refuse: ||')
    rm -f "$base.wasm"
    # shellcheck disable=SC2086
    wt 90 "$M" build "$src" --target wasm32-wasi -o "$base.wasm" "$@" $FLAGS 2> "$base.got" >/dev/null
    got_rc=$?
    if [ "$got_rc" != "$want_rc" ]; then
      verdict="FAIL (rc $got_rc, want $want_rc)"
    elif ! cmp -s "$base.got" "$out"; then
      verdict="FAIL (refusal text)"
    fi
    ;;
  *)
    verdict="FAIL (no marker)"
    ;;
  esac

  # dump-ir pins: dead modules must not appear, live ones must
  if [ -z "$verdict" ] && { [ -f "tests/lang/params/$name.irno" ] || [ -f "$dir/main.irno" ] ||
       [ -f "tests/lang/params/$name.irhas" ] || [ -f "$dir/main.irhas" ]; }; then
    rm -f "$base.ir.wasm"
    # shellcheck disable=SC2086
    wt 90 "$M" build "$src" --target wasm32-wasi -o "$base.ir.wasm" "$@" $FLAGS --dump-ir 2> "$base.ir" >/dev/null
    if [ "$dir_fix" = 1 ] && [ -f "$dir/main.irno" ]; then irno="$dir/main.irno"; else irno="tests/lang/params/$name.irno"; fi
    if [ "$dir_fix" = 1 ] && [ -f "$dir/main.irhas" ]; then irhas="$dir/main.irhas"; else irhas="tests/lang/params/$name.irhas"; fi
    if [ -f "$irno" ]; then
      while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if grep -qE "$pat" "$base.ir"; then
          verdict="FAIL (pattern survived in the artifact: $pat)"
          break
        fi
      done < "$irno"
    fi
    if [ -z "$verdict" ] && [ -f "$irhas" ]; then
      while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if ! grep -qE "$pat" "$base.ir"; then
          verdict="FAIL (pattern missing from the artifact: $pat)"
          break
        fi
      done < "$irhas"
    fi
  fi

  # fmt roundtrip on the mirror lane (params are plain consts to fmt)
  if [ -z "$verdict" ]; then
    if [ "$dir_fix" = 1 ]; then fmtsrc="$dir/zz_fmt_tmp.rho"; else fmtsrc="$G/$name.fmt1.rho"; fi
    wt 60 "$M" fmt "$src" 2>/dev/null > "$fmtsrc"
    wt 60 "$M" fmt "$fmtsrc" 2>/dev/null > "$base.fmt2"
    if ! cmp -s "$fmtsrc" "$base.fmt2"; then
      verdict="FAIL (fmt roundtrip)"
    fi
    rm -f "$fmtsrc"
  fi

  if [ -n "$verdict" ]; then
    echo "$name: $verdict"
    fails=$((fails + 1))
  fi
  unset SETS
done

echo "params: $((total - fails))/$total ok"
[ "$fails" = 0 ]
