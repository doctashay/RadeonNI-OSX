#!/bin/sh
# On the G5: play a clip in QuickTime Player for about 25 s and show what the card did: whether the kext received video decoder
# streams (kind U; only the hardware decoder makes them), the decoder's fence counter before and during, Player's CPU use, and
# whether the QuickTime component declined the clip (then QuickTime used Apple's software decoder: look for "declined" in
# `grep RadeonNIAVC /var/log/system.log`). Opens a Player window. Needs ~/iblog (tools/guest/iblog.c) and rdnuvd.
#   sh gpucheck.sh /path/to/clip.mp4
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT
clip=$1
rm -f /tmp/rdnavc.debug
~/iblog on > /dev/null
echo "UVD_CONTEXT_ID before: $(~/uvd-msg/rdnuvd.fw regs 2>&1 | grep CONTEXT_ID | awk '{print $2}')"
open -a "QuickTime Player" "$clip"
sleep 8
osascript -e 'tell application "QuickTime Player" to play document 1'
sleep 4
pid=$(ps -axww -o pid,command | grep -a "QuickTime Player.app/Contents/MacOS" | grep -v grep | awk '{print $1}' | head -1)
echo "-- while playing: Player's CPU use, three samples"
for i in 1 2 3; do ps -o pid,%cpu,rss,command -p $pid | tail -1 | cut -c1-90; sleep 2; done
echo "-- the machine: $(top -l 2 -n 0 2>/dev/null | grep -a 'CPU usage' | tail -1)"
~/iblog dump > /dev/null
echo "UVD_CONTEXT_ID during:  $(~/uvd-msg/rdnuvd.fw regs 2>&1 | grep CONTEXT_ID | awk '{print $2}')"
sleep 3
osascript -e 'tell application "QuickTime Player" to quit' >/dev/null 2>&1
sleep 3
ioreg -c RadeonNIAccel -l -w0 > /tmp/ioreg-accel.txt 2>&1
awk '/"IBLog"/ {p=1} p {print} p && /"$/ {exit}' /tmp/ioreg-accel.txt > /tmp/ibdump.txt
echo "-- the kext's record (last 160 entries): kinds and rate"
awk '$2=="U" || $2=="3" || $2=="f" {n[$2]++; if ($2=="U") { if (!t0) t0=$1; t1=$1; u++ } } END { printf "   video decoder streams (U): %d, 3D command buffers: %d, framebuffer calls: %d\n", n["U"], n["3"], n["f"]; if (u > 1) printf "   decoder streams per second over %.1f s: %.1f\n", (t1-t0)/1000, u/((t1-t0)/1000) }' /tmp/ibdump.txt
~/iblog off > /dev/null
echo "-- kernel log lines of the run:"
grep -a -E "RadeonNI: (UVD|uvd)" /var/log/system.log 2>/dev/null | tail -4 | cut -c1-110
echo "== finished"
