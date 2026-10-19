#!/usr/bin/env python3
# Check video/rdnvd_h264.c on the host: for each vector the picture types in the order its DPB
# outputs them must be the order ffmpeg shows them in (ffprobe -show_frames), and every decoded
# picture must be output once and released.
#   scripts/uvd-parse-check.py <h264dump binary> <vector dir> [names...]
import re, subprocess, sys

dump, vecdir = sys.argv[1], sys.argv[2]
names = sys.argv[3:]
if not names:
    import glob, os
    names = sorted(os.path.basename(f)[:-5] for f in glob.glob(vecdir + "/*.h264"))
bad = 0
for n in names:
    path = f"{vecdir}/{n}.h264"
    p = subprocess.run([dump, path], capture_output=True, text=True)
    types = {}
    out = []
    for line in p.stdout.splitlines():
        m = re.match(r"decode #(\d+): ([IPB])", line)
        if m:
            types[int(m.group(1)) + 1] = m.group(2)
        m = re.match(r"output #\d+ \(decoded #(\d+)\)", line)
        if m:
            out.append(types[int(m.group(1))])
    f = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
                        "-show_entries", "frame=pict_type", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    ref = [l.split(",")[0].strip() for l in f.stdout.splitlines() if l[:1] in "IPB"]
    ok = out == ref and p.returncode == 0
    print(f"{n:8} {'ok ' if ok else 'BAD'} {len(out)} pictures; {p.stderr.strip().splitlines()[-1] if p.stderr.strip() else ''}")
    if not ok:
        bad += 1
        print("  mine  :", "".join(out))
        print("  ffmpeg:", "".join(ref))
sys.exit(1 if bad else 0)
