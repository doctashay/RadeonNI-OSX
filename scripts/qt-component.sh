#!/bin/bash
# The QuickTime component (quicktime/): build it on the G5 (its Xcode gcc and Rez), install, remove.
#
#   scripts/qt-component.sh build      send the sources, rdnvd.h and librdnvd.dylib to ~/qt-build on the G5 and make
#   scripts/qt-component.sh get        copy the built bundle to build/qt/ on this machine
#   scripts/qt-component.sh install    put it in /Library/QuickTime on the G5 (this changes how QuickTime decodes H.264
#                                      for every program: ask the user first); the old one, if any, is kept
#   scripts/qt-component.sh remove     take it out of /Library/QuickTime again
#
# Run from the server (scripts/mac.sh is how the G5 is reached).
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mac() { "$root/scripts/mac.sh" g5 "$@"; }
lib=$(ls "$root"/third_party/mesa-*/build-darwin/src/gallium/targets/rdn/librdnvd.dylib | tail -n 1)

case "${1:-}" in
build)
    mac ssh 'rm -rf ~/qt-build && mkdir -p ~/qt-build'
    tar -C "$root/quicktime" -cf - . | mac ssh 'tar -C ~/qt-build -xf -'
    mac put "$root/mesa/target/rdnvd.h" '~/qt-build/rdnvd.h'
    # the library keeps the bundle's copy next to the executable: @loader_path, since Tiger has no @rpath
    mkdir -p "$root/build/qt"
    cp "$lib" "$root/build/qt/librdnvd.dylib"
    "$root/scripts/darwin.sh" powerpc-apple-darwin8-install_name_tool -id @loader_path/librdnvd.dylib \
        "$root/build/qt/librdnvd.dylib"
    mac put "$root/build/qt/librdnvd.dylib" '~/qt-build/librdnvd.dylib'
    mac ssh 'cd ~/qt-build && make 2>&1 | tail -40 && ls -lR RadeonNIAVC.component | head -30'
    ;;
get)
    mkdir -p "$root/build/qt"
    mac ssh 'cd ~/qt-build && tar -cf - RadeonNIAVC.component' | tar -C "$root/build/qt" -xf -
    ls -lR "$root/build/qt" | head -20
    ;;
install)
    mac sudo 'set -e; D=/Library/QuickTime; mkdir -p $D;
        if [ -d $D/RadeonNIAVC.component ]; then S=$(date +%Y%m%d%H%M); mv $D/RadeonNIAVC.component /tmp/RadeonNIAVC.component.before-$S; echo "old one kept as /tmp/RadeonNIAVC.component.before-$S"; fi;
        cp -R /Users/tiger/qt-build/RadeonNIAVC.component $D/; chown -R root:wheel $D/RadeonNIAVC.component; chmod -R go-w $D/RadeonNIAVC.component; ls -ld $D/RadeonNIAVC.component'
    ;;
remove)
    mac sudo 'rm -rf /Library/QuickTime/RadeonNIAVC.component; ls /Library/QuickTime'
    ;;
*)
    sed -n '2,/^set /p' "$0" | sed 's/^# \{0,1\}//' | head -14
    exit 1
    ;;
esac
