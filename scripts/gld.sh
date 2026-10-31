#!/bin/bash
# Build and install the OpenGL driver bundle in the running Tiger guest.
#
#   scripts/gld.sh build      copy gld/ to the guest and build
#   scripts/gld.sh install    copy the bundle to /System/Library/Extensions
#   scripts/gld.sh install-mesa
#                             install the host-built bundle that has Mesa
#                             inside instead (see scripts/build-mesa.sh)
#   scripts/gld.sh uninstall  remove it from there
#   scripts/gld.sh log        the bundle's call log
#   scripts/gld.sh clearlog
#
# OpenGL loads the bundle the accelerator names (IOGLBundleName) from
# /System/Library/Extensions, so unlike the kext it has to be installed.
# It is only used while the kext announces its accelerator
# (RDN_ACCEL=1 scripts/kext.sh up).

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
gssh() { "$root/scripts/tiger.sh" ssh "$@"; }
SLE=/System/Library/Extensions
NAME=RadeonNIGLDriver

case "${1:-}" in
build)
    tar -C "$root" -cf - gld | gssh 'rm -rf ~/osx-gpu-gld && mkdir -p ~/osx-gpu-gld && tar -C ~/osx-gpu-gld -xf -'
    gssh "cd ~/osx-gpu-gld/gld && make 2>&1 && file build/$NAME.bundle/Contents/MacOS/$NAME"
    ;;
install)
    gssh "sudo rm -rf $SLE/$NAME.bundle &&
        sudo cp -R ~/osx-gpu-gld/gld/build/$NAME.bundle $SLE/ &&
        sudo chown -R root:wheel $SLE/$NAME.bundle && sudo chmod -R go-w $SLE/$NAME.bundle &&
        sudo sync && ls -ld $SLE/$NAME.bundle"
    ;;
install-mesa)
    # The bundle with Mesa inside, cross-built on the host
    # (scripts/build-mesa.sh darwin src/gallium/targets/rdn/RadeonNIGLDriver.dylib).
    bin=$(ls "$root"/third_party/mesa-*/build-darwin/src/gallium/targets/rdn/$NAME.dylib | tail -n 1)
    directory=$(dirname "$bin")
    for suffix in '' 1 2 3; do
        [ -s "$directory/$NAME$suffix.dylib" ] || {
            echo "build all four GL renderer modules first" >&2; exit 1;
        }
    done
    for suffix in '' 1 2 3; do
        module=$NAME$suffix
        gssh "mkdir -p /tmp/$module.bundle/Contents/MacOS &&
            cat > /tmp/$module.bundle/Contents/Info.plist" < <(
                sed "s/$NAME/$module/g" "$root/gld/Info.plist")
        gssh "cat > /tmp/$module.bundle/Contents/MacOS/$module &&
            chmod 755 /tmp/$module.bundle/Contents/MacOS/$module &&
            sudo mkdir -p $SLE/$module.bundle &&
            sudo cp -R /tmp/$module.bundle/Contents $SLE/$module.bundle/ &&
            sudo chown -R root:wheel $SLE/$module.bundle &&
            sudo chmod -R go-w $SLE/$module.bundle" < "$directory/$module.dylib"
    done
    gssh "sudo sync"
    ;;
uninstall)
    gssh "sudo rm -rf $SLE/$NAME.bundle && sudo sync && echo removed"
    ;;
log)
    gssh 'cat /tmp/rdngld.log 2>/dev/null || echo "no log"'
    ;;
clearlog)
    gssh 'sudo rm -f /tmp/rdngld.log'
    ;;
*)
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
