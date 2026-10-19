#!/usr/bin/env python3
# Feed damaged copies of the test vectors to the H.264 layer built with the address and undefined
# behaviour sanitizers; any report is a bug. The layer sees streams from files programs do not control.
#   gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o h264dump_asan \
#       video/tests/h264dump.c video/rdnvd_h264.c
#   scripts/uvd-parse-fuzz.py <h264dump_asan> <vector dir> [iterations [seed]]
import glob, random, subprocess, sys

dump, vecdir = sys.argv[1], sys.argv[2]
iterations = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
random.seed(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
vecs = [open(f, "rb").read() for f in sorted(glob.glob(vecdir + "/*.h264"))]
bad = 0
for it in range(iterations):
    d = bytearray(random.choice(vecs))
    mode = random.randrange(5)
    if mode == 0:
        for _ in range(random.randrange(1, 20)):
            d[random.randrange(len(d))] = random.randrange(256)
    elif mode == 1:
        d = d[:random.randrange(1, len(d))]
    elif mode == 2:
        a = random.randrange(len(d)); b = min(len(d), a + random.randrange(1, 2000)); del d[a:b]
    elif mode == 3:
        for _ in range(random.randrange(1, 50)):
            p = random.randrange(len(d)); d[p] ^= 1 << random.randrange(8)
    else:   # the parameter sets and the first slice header
        for _ in range(random.randrange(1, 8)):
            p = random.randrange(min(len(d), 200)); d[p] = random.randrange(256)
    path = f"/tmp/fuzz_{random.getrandbits(32):08x}.h264"
    open(path, "wb").write(d)
    args = [dump, "-q"] + (["-a"] if it % 5 == 0 else []) + [path]
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=60)
        failed = p.returncode not in (0, 1) or "ERROR" in p.stderr or "runtime error" in p.stderr
    except subprocess.TimeoutExpired:
        p, failed = None, True
    if failed:
        bad += 1
        print("iteration", it, "kept as", path)
        if p:
            print(p.stderr[-1200:])
        if bad > 3:
            break
    else:
        import os
        os.remove(path)
print("done:", iterations, "iterations,", bad, "failures")
sys.exit(1 if bad else 0)
