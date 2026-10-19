#!/bin/sh
# Replace only the installed RadeonNI kext binary (the personality is left as it is), keeping the
# previous binary next to it as Contents/RadeonNI.before-<tag>-<stamp>. Run with sudo from the
# directory that has the new file "RadeonNI", then restart.
#   sudo sh install-kext-binary.sh [tag]
set -e
K=/System/Library/Extensions/RadeonNI.kext
D=$(cd "$(dirname "$0")" && pwd)
TAG=${1:-update}
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -s "$D/RadeonNI" ] || { echo "no $D/RadeonNI"; exit 1; }
S=$(date +%Y%m%d%H%M)
cp "$K/Contents/MacOS/RadeonNI" "$K/Contents/RadeonNI.before-$TAG-$S"
cp "$D/RadeonNI" "$K/Contents/MacOS/RadeonNI"
chown -R root:wheel "$K"
chmod -R go-w "$K"
touch /System/Library/Extensions
echo "done; the previous binary is $K/Contents/RadeonNI.before-$TAG-$S; restart"
