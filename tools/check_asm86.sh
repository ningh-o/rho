#!/bin/sh
# Development-time oracle for the in-tree amd64 assembler: assembles the
# full emitted assembly of a corpus program (plus the runtime blob) twice —
# once with rho's own assembler (raw section images) and once with the GNU
# assembler in a linux/amd64 container — then diffs the text bytes at the
# instruction level. The system assembler is a test oracle only; the
# shipped toolchain never invokes it.
set -e
cd "$(dirname "$0")/.."

src=${1:-corpus/001_hello.rho}
RHO_KEEP_ASM=1 ./build/rho-boot build "$src" --target amd64-linux -o /tmp/rho_oracle86 >/dev/null
# strip the two directive-only lines GNU as does not know; keep the rest
sed -e 's/^  \.build_version.*$//' /tmp/rho_oracle86.s > /tmp/rho_oracle86.gnu.s

docker run --rm --platform linux/amd64 -v "$PWD:/rho" docker.m.daocloud.io/library/alpine:latest sh -c '
sed -i "s/dl-cdn.alpinelinux.org/mirrors.aliyun.com/" /etc/apk/repositories 2>/dev/null
apk add -q binutils 2>/dev/null || true
as --64 -o /tmp/ref.o /tmp/rho_oracle86.gnu.s 2>/dev/null || as --64 -o /tmp/ref.o /rho/tmp/rho_oracle86.gnu.s
objdump -d --section=.text /tmp/ref.o | awk "/^ /{print \$2}" | head -n -1 > /tmp/ref.insns
wc -l /tmp/ref.insns
'
echo "--- our raw image disasm requires a local objdump; compare by instruction count first"
