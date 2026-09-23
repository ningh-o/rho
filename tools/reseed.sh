#!/bin/sh
# tools/reseed.sh — the re-pinning ritual, scripted (docs/bootstrap.md,
# "The re-pinning ritual").
#
# The pinned seed (boot/rho-seed.wasm) is the frontier compiler: the
# mirror's own sources may use anything IT accepts. The day the mirror
# adopts a feature the seed lacks, the seed must be re-pinned to a
# compiler that speaks the new language — that is this script:
#
#   1. build the package root with the best builder available — boot
#      (build/rho-boot) while it can still parse the mirror's sources,
#      else the current pinned seed (the honest lineage since the dot
#      round took the mirror past boot's frozen grammar);
#   2. that artifact becomes boot/rho-seed.wasm — UNLESS it is already
#      byte-identical to the current pin, in which case nothing is
#      written. This is the script's idempotence: at a settled frontier
#      "the seed compiles the mirror to itself" and the ritual is a
#      verified no-op;
#   3. the pin slot in docs/bootstrap.md (date + SHA-256) and the pin
#      ledger under it are rewritten mechanically. The Provenance prose —
#      the sentence saying WHICH feature forced the re-pin — stays an
#      authored step; the script says so when it finishes;
#   4. every later tools/gate.sh run re-checks the slot against the
#      file's actual hash (the seed-chain leg calls --check below), so
#      the records cannot rot silently.
#
# The superseded pin is kept at build/gate/rho-seed-superseded.wasm for
# diffing. Every rho invocation runs under a wall-clock cap (macOS has
# no GNU timeout; the alarm survives exec).
#
# Usage:
#   tools/reseed.sh            the ritual (idempotent)
#   tools/reseed.sh --check    docs' pin slot vs the file's real SHA-256
set -u
cd "$(dirname "$0")/.."

G=build/gate
DOCS=docs/bootstrap.md
PIN=boot/rho-seed.wasm
ROOT=libs/compiler/cli.rho
mkdir -p "$G"

run_t() {
  t=$1; shift
  perl -e 'alarm shift; exec @ARGV or die "reseed: cannot exec $ARGV[0]: $!\n"' "$t" "$@"
}

# the pin slot is the docs' one "pinned <date>, SHA-256 `xxxxxxxx…`" —
# the ledger lines deliberately spell their hashes differently so this
# grep stays unambiguous
slot_sha() {
  sed -n 's/.*pinned [0-9-]*, SHA-256 `\([0-9a-f]\{8\}\)….*/\1/p' "$DOCS" | head -1
}

if [ "${1:-}" = "--check" ]; then
  [ -f "$PIN" ] || { echo "pin-check: no seed at $PIN"; exit 1; }
  want=$(slot_sha)
  [ -n "$want" ] || { echo "pin-check: no pin slot in $DOCS"; exit 1; }
  have=$(shasum -a 256 "$PIN" | cut -c1-8)
  if [ "$have" = "$want" ]; then
    echo "pin-check: ok (seed hashes to ${want}…, the pin $DOCS records)"
    exit 0
  fi
  echo "pin-check: PIN ROT — $PIN hashes to ${have}…, $DOCS records ${want}…"
  echo "  run tools/reseed.sh (or fix the pin slot if the seed is the right one)"
  exit 1
fi

# --- the ritual ---------------------------------------------------------

[ -x build/rho-boot ] || { echo "reseed: no boot at build/rho-boot (make build/rho-boot)"; exit 2; }
[ -f "$PIN" ] || { echo "reseed: no current pin at $PIN (nothing to supersede)"; exit 2; }

CAND=$G/reseed-candidate.wasm
rm -f "$CAND"

# step 1 — the best builder: boot while the mirror still fits its frozen
# grammar (a refusal is fast and clean: a parse diag, no artifact), the
# current pin otherwise
echo "reseed: probing whether boot can still build the mirror (300s cap)"
if run_t 300 ./build/rho-boot build "$ROOT" --target wasm32-wasi -o "$CAND" >/dev/null 2>&1 && [ -f "$CAND" ]; then
  BUILDER=boot-built
else
  rm -f "$CAND"
  BUILDER=seed-built
  echo "reseed: boot cannot (expected once the mirror runs past its frontier)"
  echo "reseed: the current pin builds the mirror (900s cap)"
  run_t 900 wasmtime run --dir . "$PIN" \
    build "$ROOT" --target wasm32-wasi -o "$CAND" >/dev/null 2>&1
  if [ ! -f "$CAND" ]; then
    echo "reseed: the current pin cannot build the mirror and boot cannot either —"
    echo "  the chain is broken; fix the sources before re-pinning"
    exit 2
  fi
fi

# step 2 — the idempotence gate: a settled frontier compiles to the pin
if cmp -s "$CAND" "$PIN"; then
  echo "reseed: the mirror compiles to the pin itself — already at the pin, nothing to do"
  want=$(slot_sha)
  have=$(shasum -a 256 "$PIN" | cut -c1-8)
  if [ "$have" != "$want" ]; then
    echo "reseed: PIN ROT — $PIN hashes to ${have}…, $DOCS records ${want}…; fix the slot"
    exit 1
  fi
  exit 0
fi

# step 3 — rewrite the records FIRST (a docs failure must not leave a
# half-rewritten pin), then move the artifacts into place. The slot and
# ledger replacements ride to perl through the environment: hash strings
# and backticks never touch shell quoting.
old8=$(shasum -a 256 "$PIN" | cut -c1-8)
new8=$(shasum -a 256 "$CAND" | cut -c1-8)
today=$(date '+%Y-%m-%d')
tmp="$DOCS.reseed.tmp"
NEWSLOT="pinned $today, SHA-256 \`${new8}…\`,"
LEDGER="- \`${new8}…\` — $today, $BUILDER (supersedes \`${old8}…\`)"

NEWSLOT="$NEWSLOT" LEDGER="$LEDGER" perl -pe '
  s/pinned \d{4}-\d{2}-\d{2}, SHA-256 `[0-9a-f]{8}…`?,/$ENV{NEWSLOT}/;
  print "$ENV{LEDGER}\n" if /^<!-- pin-ledger -->$/' \
  "$DOCS" > "$tmp" || { echo "reseed: docs rewrite failed"; exit 2; }

grep -qF "$NEWSLOT" "$tmp" || { echo "reseed: pin slot did not land"; rm -f "$tmp"; exit 2; }
[ "$(grep -c 'SHA-256 `' "$tmp")" = 1 ] || { echo "reseed: pin slot ambiguous — refusing"; rm -f "$tmp"; exit 2; }
grep -qF -e "$LEDGER" "$tmp" || { echo "reseed: ledger line did not land"; rm -f "$tmp"; exit 2; }

cp "$PIN" "$G/rho-seed-superseded.wasm"
cp "$CAND" "$PIN"
mv "$tmp" "$DOCS"

echo "reseed: pin moved — new seed is $BUILDER, SHA-256 $(shasum -a 256 "$PIN" | cut -d' ' -f1)"
echo "reseed: superseded pin kept at $G/rho-seed-superseded.wasm (was ${old8}…)"
echo "reseed: pin slot + ledger updated in $DOCS"
echo "reseed: NOW AUTHOR THE PROSE — one sentence in $DOCS's Provenance"
echo "  paragraph: WHICH feature the mirror adopted that the old pin lacked."
echo "reseed: then prove the new pin: sh tools/gate.sh"
exit 0
