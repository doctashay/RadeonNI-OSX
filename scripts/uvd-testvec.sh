#!/bin/sh
# Small H.264 test vectors for rdn_uvdtest: a synthetic test pattern as
# Baseline (CAVLC, one slice per picture, I and P pictures, no B) and its
# decode by ffmpeg.
#   scripts/uvd-testvec.sh <outdir> [width height frames [refs [cabac]]]
# Writes <outdir>/t.h264 and <outdir>/t.yuv (yuv420p, all the pictures).
# With "cabac" 1 the stream is Main profile (CABAC) instead.
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT
set -e
out=${1:?usage: uvd-testvec.sh <outdir> [width height frames [refs [cabac]]]}
w=${2:-128}
h=${3:-96}
frames=${4:-1}
refs=${5:-1}
cabac=${6:-0}
profile=baseline
[ "$cabac" = 1 ] && profile=main
mkdir -p "$out"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=${w}x${h}:rate=25" -frames:v "$frames" \
    -pix_fmt yuv420p -c:v libx264 -profile:v $profile \
    -x264-params "keyint=12:slices=1:bframes=0:ref=$refs:cabac=$cabac" -f h264 "$out/t.h264"
ffmpeg -hide_banner -loglevel error -y -i "$out/t.h264" -pix_fmt yuv420p -f rawvideo "$out/t.yuv"
ls -l "$out/t.h264" "$out/t.yuv"
