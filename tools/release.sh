#!/bin/zsh
# release.sh — assemble the v0.1.0 release (T6.2): the self-hosted
# compiler in its app configuration, zipped as rho.wasm beside a short
# usage README and the license, with SHA256SUMS next to the zip.
#
# Everything here is reproducible from the repo at the tag: the binary
# is boot's direct build output (no wasm-opt — the shrinker is the
# SITE asset law, and a foreign wasm-opt version would break the
# published byte hashes), built by the same command the gate's canary
# leg byte-checks. Run from a clean tree at the tag:
#
#   zsh tools/release.sh
#
# Outputs into build/release/:
#   rho-0.1.0-wasm32-wasi.zip
#   SHA256SUMS
set -eu
cd "$(dirname "$0")/.."
REL=build/release
STAGE=$REL/rho-0.1.0-wasm32-wasi

rm -rf "$REL" && mkdir -p "$STAGE"

echo "== building the compiler (app face, std baked in)"
zsh tools/build-app-artifact.sh "$STAGE/rho.wasm"

echo "== staging README + LICENSE"
cp RELEASE.md "$STAGE/README.md"
cp LICENSE "$STAGE/LICENSE"

echo "== zip + sums"
# a reproducible zip: fixed timestamps and a fixed order — the archive
# bytes are a function of the staged content alone, so the published
# SHA256SUMS hold on any machine, any day
node - "$REL" <<'EOF'
const { execSync } = require("node:child_process");
const { createWriteStream } = require("node:fs");
const { join } = require("node:path");
const { deflateRawSync } = require("node:zlib");
const rel = process.argv[2];
const root = join(rel, "rho-0.1.0-wasm32-wasi");
const files = ["LICENSE", "README.md", "rho.wasm"]; // fixed order
// local file header + central directory, crc32 by hand (zlib's crc32
// lives behind an async API in some node versions; 8 lines beat that)
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
const chunks = [];
const central = [];
let offset = 0;
const DOS = { time: 0, date: (1 << 5) | 1 }; // 1980-01-01 00:00:00
for (const name of files) {
  const data = require("node:fs").readFileSync(join(root, name));
  const comp = deflateRawSync(data, { level: 9 });
  const cname = Buffer.from(`rho-0.1.0-wasm32-wasi/${name}`, "utf8");
  const crc = crc32(data);
  const lfh = Buffer.alloc(30);
  lfh.writeUInt32LE(0x04034b50, 0);
  lfh.writeUInt16LE(20, 4); // version needed
  lfh.writeUInt16LE(0, 6); // flags
  lfh.writeUInt16LE(8, 8); // deflate
  lfh.writeUInt16LE(DOS.time, 10);
  lfh.writeUInt16LE(DOS.date, 12);
  lfh.writeUInt32LE(crc, 14);
  lfh.writeUInt32LE(comp.length, 18);
  lfh.writeUInt32LE(data.length, 22);
  lfh.writeUInt16LE(cname.length, 26);
  chunks.push(lfh, cname, comp);
  const cdh = Buffer.alloc(46);
  cdh.writeUInt32LE(0x02014b50, 0);
  cdh.writeUInt16LE(20, 4);
  cdh.writeUInt16LE(20, 6);
  cdh.writeUInt16LE(0, 8);
  cdh.writeUInt16LE(8, 10);
  cdh.writeUInt16LE(DOS.time, 12);
  cdh.writeUInt16LE(DOS.date, 14);
  cdh.writeUInt32LE(crc, 16);
  cdh.writeUInt32LE(comp.length, 20);
  cdh.writeUInt32LE(data.length, 24);
  cdh.writeUInt16LE(cname.length, 28);
  cdh.writeUInt32LE(offset, 42);
  central.push(cdh, cname);
  offset += lfh.length + cname.length + comp.length;
}
const cdBuf = Buffer.concat(central);
const eocd = Buffer.alloc(22);
eocd.writeUInt32LE(0x06054b50, 0);
eocd.writeUInt16LE(files.length, 8);
eocd.writeUInt16LE(files.length, 10);
eocd.writeUInt32LE(cdBuf.length, 12);
eocd.writeUInt32LE(offset, 16);
require("node:fs").writeFileSync(
  join(rel, "rho-0.1.0-wasm32-wasi.zip"),
  Buffer.concat([...chunks, cdBuf, eocd]),
);
EOF
cd "$REL"
shasum -a 256 rho-0.1.0-wasm32-wasi.zip > SHA256SUMS
cd ../..

echo "== smoke probe on the zipped binary"
TMP=$(mktemp -d)
unzip -q "$REL/rho-0.1.0-wasm32-wasi.zip" -d "$TMP"
mkdir -p "$TMP/room"
cp "$TMP/rho-0.1.0-wasm32-wasi/rho.wasm" "$TMP/room/"
cat > "$TMP/room/main.rho" <<'EOF'
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
  printf("hello, {}\n", name);
  return 0;
}
EOF
cat > "$TMP/room/mode" <<'EOF'
build
EOF
(cd "$TMP/room" && wasmtime run --dir . rho.wasm > out.wat 2>/dev/null)
wat2wasm "$TMP/room/out.wat" -o "$TMP/room/prog.wasm"
got=$(echo "release" | perl -e 'alarm 20; exec @ARGV' -- wasmtime "$TMP/room/prog.wasm" 2>/dev/null)
[ "$got" = "hello, release" ] \
  || { echo "SMOKE RED: the release binary misbehaves (got: $got)"; exit 1; }
rm -rf "$TMP"
echo "   zipped binary runs (greet probe green)"

echo "release ready:"
ls -la "$REL" | awk '{print "   " $5, $9}'
cat "$REL/SHA256SUMS"
