#!/bin/sh
# Small H.264 test vectors for rdn_uvdtest and h264dump: a synthetic test pattern encoded by x264
# and its decode by ffmpeg (yuv420p, all the pictures, in display order).
#   scripts/uvd-testvec.sh <outdir> <name> [width height frames]
# Writes <outdir>/<name>.h264 and <outdir>/<name>.yuv. Names (what the stream has):
#   i        one IDR picture, Baseline
#   p        Baseline I and P, 3 references
#   cabac    Main, CABAC, 2 references
#   high     High: CABAC, 8x8 transform, 4 references
#   b        High with B pictures (pyramid, weighted), 4 references, CABAC
#   bcavlc   High with B pictures, CAVLC
#   mbaff    High, interlaced coding (MBAFF), B pictures
#   slices   High, 4 slices per picture, B pictures
#   weightp  High, weighted prediction on a fade
#   cqm      High, a custom scaling matrix (not symmetric), B pictures
#   cqmjvt   High, the JVT default scaling matrices
#   opengop  High, open GOP (I pictures that are not IDR), B pictures
#   noloop   High, deblocking off, constrained intra
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT
set -e
out=${1:?usage: uvd-testvec.sh <outdir> <name> [width height frames]}
name=${2:?name}
w=${3:-128}
h=${4:-96}
frames=${5:-40}
mkdir -p "$out"
src="testsrc2=size=${w}x${h}:rate=25"
vf=
prof=high
x="keyint=24:min-keyint=6"
case $name in
i)       prof=baseline; frames=1; x="keyint=1:slices=1:bframes=0:ref=1:cabac=0" ;;
p)       prof=baseline; x="$x:bframes=0:ref=3:cabac=0" ;;
cabac)   prof=main; x="$x:bframes=0:ref=2:cabac=1" ;;
high)    x="$x:bframes=0:ref=4:cabac=1:8x8dct=1" ;;
b)       x="$x:bframes=3:b-pyramid=normal:ref=4:weightb=1:cabac=1:8x8dct=1" ;;
bcavlc)  x="$x:bframes=3:b-pyramid=normal:ref=4:cabac=0:8x8dct=1" ;;
mbaff)   x="$x:bframes=2:ref=3:interlaced=1:tff=1:cabac=1" ;;
slices)  x="$x:bframes=2:ref=3:slices=4:cabac=1" ;;
weightp) x="$x:bframes=0:ref=2:weightp=2:cabac=1"; vf="fade=in:0:30" ;;
cqm)     x="$x:bframes=2:ref=3:cqmfile=$out/$name.cqm"
	 cat > "$out/$name.cqm" <<CQM
INTRA4X4_LUMA = 6,12,19,26,12,19,26,31,19,26,31,35,26,31,35,39
INTRA4X4_CHROMA = 8,13,20,27,13,20,27,32,20,27,32,36,27,32,36,40
INTER4X4_LUMA = 9,14,21,28,15,22,29,33,22,28,32,36,29,33,37,41
INTER4X4_CHROMA = 10,15,22,29,16,23,30,34,23,29,33,37,30,34,38,42
INTRA8X8_LUMA = 6,10,13,16,18,23,25,27,10,11,16,18,23,25,27,29,13,16,18,23,25,27,29,31,16,18,23,25,27,29,31,33,18,23,25,27,29,31,33,36,23,25,27,29,31,33,36,38,25,27,29,31,33,36,38,40,27,29,31,33,36,38,40,42
INTER8X8_LUMA = 9,13,15,17,19,21,22,24,13,13,17,19,21,22,24,25,15,17,19,21,22,24,25,27,17,19,21,22,24,25,27,28,19,21,22,24,25,27,28,30,21,22,24,25,27,28,30,32,22,24,25,27,28,30,32,33,24,25,27,28,30,32,33,35
CQM
	 ;;
cqmjvt)  x="$x:bframes=2:ref=3:cqm=jvt" ;;
opengop) x="$x:bframes=3:ref=3:open-gop=1" ;;
noloop)  x="$x:bframes=2:ref=3:no-deblock=1:constrained-intra=1" ;;
*) echo "unknown vector $name" >&2; exit 1 ;;
esac
[ -n "$vf" ] && vf="-vf $vf"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$src" $vf -frames:v "$frames" -pix_fmt yuv420p \
    -c:v libx264 -profile:v $prof -x264-params "$x" -f h264 "$out/$name.h264"
ffmpeg -hide_banner -loglevel error -y -i "$out/$name.h264" -pix_fmt yuv420p -f rawvideo "$out/$name.yuv"
echo "$name: $(stat -c %s "$out/$name.h264") bytes of stream, $(stat -c %s "$out/$name.yuv") of yuv"
