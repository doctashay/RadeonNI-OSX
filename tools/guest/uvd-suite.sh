#!/bin/sh
# On the G5: run rdn_uvdtest over every vector in a directory (name.h264 with name.yuv next to it)
# and print one line each and the totals. Nothing is shown on the screen.
#   sh uvd-suite.sh <rdn_uvdtest> <vector dir> [names...]
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT
test="$1"; dir="$2"; shift 2
if [ $# -eq 0 ]; then
	set -- $(cd "$dir" && ls *.h264 | sed 's/\.h264$//')
fi
pass=0; fail=0
for v in "$@"; do
	out=$("$test" "$dir/$v.h264" "$dir/$v.yuv" 2>&1 | grep -v "malloc")
	if echo "$out" | grep -q '^PASS'; then
		pass=$((pass + 1)); res=PASS
	else
		fail=$((fail + 1)); res=FAIL
	fi
	echo "$res $v: $(echo "$out" | grep -v '^PASS' | grep -v '^FAIL' | tail -1)"
done
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
