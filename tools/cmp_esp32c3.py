#!/usr/bin/env python3
"""Compare boot vs self-stage0 esp32c3 outputs: .s text AND flat image."""
import os
import subprocess
import sys

RHO = "/Users/ning/Developer/ningh-o/rho"
BOOT = f"{RHO}/build/rho-boot"
STAGE0 = "/tmp/self_stage0"

files = []
for d in ("corpus", "tests/frontend"):
    for f in sorted(os.listdir(f"{RHO}/{d}")):
        if f.endswith(".rho") and f != "mod_aux.rho":
            files.append(f"{d}/{f}")

passed, failed = [], []
for f in files:
    tag = f.replace("/", "_")
    bb = f"/tmp/rvb_{tag}.img"
    ss = f"/tmp/rvs_{tag}.img"
    for p in (bb, ss, bb + ".s", ss + ".s"):
        if os.path.exists(p):
            os.remove(p)
    r1 = subprocess.run([BOOT, "build", f, "-o", bb, "--target", "esp32c3"],
                        cwd=RHO, capture_output=True)
    r2 = subprocess.run([STAGE0, "build", f, "-o", ss, "--target", "esp32c3"],
                        cwd=RHO, capture_output=True)
    if not os.path.exists(ss) or not os.path.exists(bb):
        if not os.path.exists(bb):
            continue  # boot itself cannot build this file: not a parity case
        err = r2.stderr.decode()
        tail = [l for l in err.split("\n") if l and "panic" in l.lower()][-1:]
        failed.append((f, "BUILD-FAIL: " + (tail[0] if tail else "?")))
        continue
    same_img = open(bb, "rb").read() == open(ss, "rb").read()
    same_txt = open(bb + ".s", "rb").read() == open(ss + ".s", "rb").read()
    if same_img and same_txt:
        passed.append(f)
    else:
        failed.append((f, "DIFF text" if not same_txt else "DIFF image"))

print(f"esp32c3 parity: {len(passed)} ok, {len(failed)} differ")
for f, why in failed:
    print(f"  {f}: {why}")
sys.exit(1 if failed else 0)
