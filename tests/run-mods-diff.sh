#!/bin/zsh
# multi-module differential (T8): the same module-tree program through
# boot (files on disk) and through the self-hosted compiler (the MODS
# build parameter) must behave identically — stdout and exit code. The
# single-file differential (run-diff.sh) grew up first; the module
# faces are the T8 campaign's surface, and every fix there lands with a
# program here.
#
# The dotted-canon face (std.json — a package under the reserved std/)
# is exercised by the corpus-repo legs once the tree loads; user trees
# here cannot take the std name (module-system.md §7).
set -u
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
RHO=${RHO:-./build/rho}
FAILED=0

mods_diff_one() { # name, src, tree-dir (main.rho written from src)
  local name=$1 src=$2 tree=$3
  local mods="" f
  for f in $(cd $tree && find . -name '*.rho' ! -name 'main.rho' | sort); do
    mods="$mods@MOD@ ${f#./}
$(cat $tree/$f)
"
  done
  printf '%s\n' "$src" > $tree/main.rho
  local bgot brc
  bgot=$("$RHO" run $tree/main.rho 2>/dev/null)
  brc=$?
  if ! "$RHO" build libs/compiler/main.rho -o $T/md-$name.wasm \
      --set "SRC=$src" --set "MODS=$mods" >$T/md-$name.build 2>&1; then
    echo "FAIL mods-diff/$name: boot could not build the compiler"
    head -3 $T/md-$name.build
    FAILED=1
    return
  fi
  wasmtime $T/md-$name.wasm >$T/md-$name.wat 2>/dev/null
  if ! wat2wasm $T/md-$name.wat -o $T/md-$name.self.wasm 2>$T/md-$name.w2w; then
    echo "FAIL mods-diff/$name: the self-hosted output does not assemble"
    head -3 $T/md-$name.w2w
    FAILED=1
    return
  fi
  local sgot src_rc
  sgot=$(perl -e 'alarm 10; exec @ARGV' -- wasmtime $T/md-$name.self.wasm 2>/dev/null)
  src_rc=$?
  if [ "$brc" -eq "$src_rc" ] && [ "$bgot" = "$sgot" ]; then
    echo "  mods-diff/$name: boot==self rc=$brc [$bgot]"
  else
    echo "FAIL mods-diff/$name: boot rc=$brc=[$bgot] self rc=$src_rc=[$sgot]"
    FAILED=1
  fi
}

# --- T8.8's first face: a module calling its OWN type's associated fn
#     (Big.build) — the qualifier used to leave the bare dotted call
#     alone and the checker met the fn under its qualified name only
M=$T/assoc
mkdir -p $M
cat > $M/mathutil.rho <<'EOF'
pub struct Big {
  v: u64,
}

fn Big.build(v: u64) -> *Big {
  return new Big { v: v };
}

pub fn use_it() -> u64 {
  let mut n: *Big = Big.build(3);
  return n.v;
}
EOF
mods_diff_one assoc-call \
  'use mathutil;
fn main() -> i32 {
  printf("{}\n", mathutil.use_it());
  return 0;
}' $T/assoc

# --- the facade body calling its own type's associated fn, plus a
#     chained file module behind it (the json ObjB.begin2 shape)
M=$T/facade
mkdir -p $M/geom
cat > $M/geom/lib.rho <<'EOF'
use points;

pub struct ObjB {
  v: u64,
}

pub fn ObjB.begin2() -> *ObjB {
  return new ObjB { v: 7 };
}

pub fn mk() -> *ObjB {
  return ObjB.begin2();
}

pub fn indirect() -> u64 {
  return points.base() + mk().v;
}
EOF
cat > $M/geom/points.rho <<'EOF'
pub fn base() -> u64 {
  return 30;
}
EOF
mods_diff_one facade-assoc-call \
  'use geom;
fn main() -> i32 {
  printf("{} {}\n", geom.mk().v, geom.indirect());
  return 0;
}' $T/facade

# --- T8.6's face: an interior file importing its OWN facade (the §2
#     entry-base retry — geom/inner names geom, the package it lives in)
M=$T/cycle
mkdir -p $M/geom
cat > $M/geom/lib.rho <<'EOF'
pub use inner.peri;

pub fn area(w: i32, h: i32) -> i32 {
  return w * h;
}
EOF
cat > $M/geom/inner.rho <<'EOF'
use geom;

pub fn peri(w: i32, h: i32) -> i32 {
  return geom.area(w, h) + 1;
}
EOF
mods_diff_one interior-facade-cycle \
  'use geom;
fn main() -> i32 {
  printf("{}\n", geom.peri(3, 4));
  return 0;
}' $T/cycle

if [ "$FAILED" -eq 0 ]; then
  echo "mods-differential: ok (boot == self-hosted on the module faces)"
else
  exit 1
fi
