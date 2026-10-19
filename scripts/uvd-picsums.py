#!/usr/bin/env python3
"""Check a log of the QuickTime component's DrawBand lines against ffmpeg's decode of the same clip.

  scripts/uvd-picsums.py log.txt picsums.txt [--fps 25] [--timescale 1000]

log.txt: `grep RadeonNIAVC /var/log/system.log` of the G5 (lines "DrawBand: display T picture P sum S zero Z%");
picsums.txt: the output of tools/uvd-picsums on the reference yuv. The picture drawn for display time T is number
T * fps / timescale. Prints, per process and per instance of the component (pictures are told apart by the
sequence of calls), how many pictures were right, wrong or all zero, and the first few wrong ones.

Copyright (c) 2026 kouta-kun and Claude
SPDX-License-Identifier: MIT
"""
import re
import sys
from collections import defaultdict

args = sys.argv[1:]
fps, timescale = 25.0, 1000.0
paths = []
while args:
    a = args.pop(0)
    if a == "--fps":
        fps = float(args.pop(0))
    elif a == "--timescale":
        timescale = float(args.pop(0))
    else:
        paths.append(a)
if len(paths) != 2:
    sys.exit(__doc__)

ref = {}
for line in open(paths[1]):
    n, s = line.split()
    ref[int(n)] = s

pat = re.compile(r"\[(\d+)\]: RadeonNIAVC: DrawBand: display (-?\d+) picture \S+ sum ([0-9a-f]+) zero (\d+)%")
per = defaultdict(lambda: [0, 0, 0, []])  # pid -> right, wrong, zero, examples
for line in open(paths[0], errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    pid, disp, s, zero = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))
    n = int(round(disp * fps / timescale))
    r = per[pid]
    if zero >= 90:
        r[2] += 1
        status = "all zero (green)"
    elif ref.get(n) == s:
        r[0] += 1
        continue
    else:
        r[1] += 1
        status = "differs from picture %d" % n
    if len(r[3]) < 6:
        r[3].append("  display %d: %s (zero %d%%)" % (disp, status, zero))

bad = 0
for pid, (right, wrong, zero, ex) in sorted(per.items()):
    print("process %d: %d pictures drawn: %d right, %d wrong, %d all zero" % (pid, right + wrong + zero, right, wrong, zero))
    for e in ex:
        print(e)
    bad += wrong + zero
print("PASS" if per and not bad else "FAIL")
sys.exit(1 if bad or not per else 0)
