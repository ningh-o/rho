#!/bin/zsh
# build-app-artifact.sh — the site's runtime-compile artifact.
#
# The app face (libs/compiler/main.rho) reads /main.rho (plus a /mode
# marker) at runtime and compiles it; the module tree rides the baked
# MODS_APP const. This bakes the reserved std/ tree in and leaves SRC
# at its default (the absent-file self-compile path — §7's chain law
# wants the chain's own builds byte-identical, which an empty
# MODS_APP preserves there). Boot's build output IS the artifact — it
# assembles through wat2wasm itself (one assembler, never a second).
#
# Usage: tools/build-app-artifact.sh <out.wasm>
set -u
OUT=${1:?usage: build-app-artifact.sh <out.wasm>}
# the smoke probe runs from a tmp room — the out path must survive it
OUT=$(perl -e 'use Cwd "abs_path"; print abs_path($ARGV[0])' "$OUT" 2>/dev/null \
  || perl -e 'use Cwd "abs_path"; use File::Basename; my $d = dirname(abs_path($ARGV[0])); mkdir $d unless -d $d; print abs_path("$ARGV[0]")' "$OUT")
RHO=${RHO:-./build/rho}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

CMODS=""
for f in std/*.rho std/*/*.rho std/*/*/*.rho; do
  [ -f "$f" ] && CMODS="$CMODS@MOD@ $f
$(cat "$f")
"
done

$RHO build libs/compiler/main.rho -o "$T/m-app.wasm" \
    --set "MODS_APP=$CMODS" >"$T/build.log" 2>&1 \
  || { echo "FAIL: boot could not build the app face"; head -5 "$T/build.log"; exit 1; }
cp "$T/m-app.wasm" "$OUT"

# the smoke probe: a std-using program through the runtime face
mkdir -p "$T/room"
cat > "$T/room/main.rho" <<'EOF'
use std.io;
fn main() -> i32 {
  let got = io.read_line();
  let name: string = match got {
    Result.Ok(inner) => match inner {
      Option.Some(s) => s,
      Option.None => "",
    },
    Result.Err(_) => "",
  };
  printf("hi, {}\n", name);
  return 0;
}
EOF
cd "$T/room"
wasmtime run --dir . "$OUT" >"$T/probe.wat" 2>"$T/probe.log" \
  || { echo "FAIL: the runtime face refused the probe"; head -5 "$T/probe.log"; exit 1; }
wat2wasm "$T/probe.wat" -o "$T/prog.wasm" 2>/dev/null \
  || { echo "FAIL: the probe WAT does not assemble"; exit 1; }
got=$(echo 'rho' | perl -e 'alarm 20; exec @ARGV' -- wasmtime "$T/prog.wasm" 2>/dev/null)
[ "$got" = "hi, rho" ] \
  || { echo "FAIL: the probe ran but printed [$got]"; exit 1; }
echo "app artifact: $OUT ($(wc -c <"$OUT" | tr -d ' ') bytes) — runtime face probe green"
