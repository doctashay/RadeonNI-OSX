#!/bin/bash
# The framebuffer kext for Mac OS X 10.5 (Leopard, PowerPC). EXPERIMENTAL:
# nothing here has run on a 10.5 Mac.
#
#   scripts/leopard.sh build     build kext/RadeonNI with LEOPARD=1 in the
#                                Tiger guest (its own directory there) and
#                                bring the bundle to build/leopard/
#   scripts/leopard.sh check     compare the kext's undefined symbols with
#                                what Leopard's kernel exports (xnu-1228.15.4,
#                                fetched into third_party/leopard-exports)
#   LEOPARD_NATIVE=1 scripts/leopard.sh build|check|package
#                                the same inside the Leopard guest (Xcode 3.1.4)
#   scripts/leopard.sh package-accel   as package, plus the 3D engine's userland
#                                (OpenGL bundle, 2D plug-in), the microcode and
#                                the file LEOPARD-ACCEL that lets install.sh
#                                take --accel on 10.5. EXPERIMENTAL.
#   scripts/leopard.sh package   build/RadeonNI-10.5-<date>-<commit>.zip:
#                                kext, install.sh, LICENSE, README, the file
#                                LEOPARD that makes install.sh accept 10.5
#
# Why a separate build: Tiger's headers leave two vtable slots reserved that
# Leopard made real methods (IOService::systemWillShutdown, IOUserClient::
# externalMethod); kext/RadeonNI/leopard.s defines the old names. The Tiger
# kext does not get that file. What was compared and found equal (class
# layouts of IOFramebuffer, IOService, IOUserClient, IOPCIDevice, ...) is in
# docs/LEOPARD.md.
#
# No acceleration: Apple's OpenGL driver interface and the window server
# differ on 10.5; the package has no GL bundle, 2D plug-in or microcode.
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
# LEOPARD_NATIVE=1 builds inside the real Leopard guest
# (scripts/leopard-guest.sh) instead of cross-building in the Tiger guest;
# the result goes to build/leopard-native/ so the two can be compared.
if [ "${LEOPARD_NATIVE:-0}" = 1 ]; then
    out=$root/build/leopard-native
    gssh() { "$root/scripts/leopard-guest.sh" ssh "$@"; }
else
    out=$root/build/leopard
    gssh() { "$root/scripts/tiger.sh" ssh "$@"; }
fi
exports=$root/third_party/leopard-exports
tag=xnu-1228.15.4
gdir=osx-gpu-leopard

build() {
    tar -C "$root" -cf - hw kext | gssh "rm -rf ~/$gdir && mkdir -p ~/$gdir && tar -C ~/$gdir -xf -"
    gssh "cd ~/$gdir/kext/RadeonNI && make LEOPARD=1 2>&1 | tail -5 && file build-leopard/RadeonNI.kext/Contents/MacOS/RadeonNI"
    rm -rf "$out"
    mkdir -p "$out"
    gssh "tar -C ~/$gdir/kext/RadeonNI/build-leopard -cf - RadeonNI.kext" | tar -C "$out" --warning=no-timestamp -xf -
    [ -f "$out/RadeonNI.kext/Contents/MacOS/RadeonNI" ] || { echo "no kext came back from the guest" >&2; exit 1; }
    if grep -q '<key>VBIOS</key>' "$out/RadeonNI.kext/Contents/Info.plist"; then
        echo "the built kext already carries a VBIOS; refusing" >&2
        exit 1
    fi
    gssh "nm -u ~/$gdir/kext/RadeonNI/build-leopard/RadeonNI.kext/Contents/MacOS/RadeonNI" > "$out/undefined.txt"
    echo "built: $out/RadeonNI.kext ($(wc -l < "$out/undefined.txt" | tr -d ' ') undefined symbols)"
}

fetch_exports() {
    mkdir -p "$exports"
    local base=https://raw.githubusercontent.com/apple-oss-distributions/xnu/$tag/config
    for n in System6.0 System6.0.ppc IOKit IOKit.ppc Libkern Libkern.ppc BSDKernel BSDKernel.ppc \
             Mach Mach.ppc Unsupported Unsupported.ppc; do
        [ -s "$exports/$n.exports" ] ||
            curl -sfL "$base/$n.exports" -o "$exports/$n.exports" || { echo "cannot fetch $n.exports" >&2; exit 1; }
    done
}

check() {
    [ -s "$out/undefined.txt" ] || { echo "run build first" >&2; exit 1; }
    # Relocations kld cannot take (it crashed kextd on a 10.5.8 G5).
    "$root/scripts/check-kext-relocs.sh" "$out/RadeonNI.kext/Contents/MacOS/RadeonNI"
    fetch_exports
    python3 -I - "$exports" "$out/undefined.txt" <<'PY'
import re, sys, collections
d, und = sys.argv[1], sys.argv[2]
have = set()
for n in ('System6.0', 'System6.0.ppc', 'IOKit', 'IOKit.ppc', 'Libkern', 'Libkern.ppc',
          'BSDKernel', 'BSDKernel.ppc', 'Mach', 'Mach.ppc', 'Unsupported', 'Unsupported.ppc'):
    for l in open('%s/%s.exports' % (d, n), errors='replace'):
        l = l.strip()
        if l and not l.startswith('#'):
            have.add(l.split(':')[0])      # "name:target" is an alias
syms = [l.strip() for l in open(und) if l.strip()]
FAMILY = ('IOPCI', 'IOAGP', 'IOFramebuffer', 'IOGraphics', 'IODisplay', 'IOFB', 'IOAccelerator')


def cls(s):
    m = re.match(r'__Z(?:T[VIS])?N?K?(\d+)', s)   # also vtable and typeinfo names
    return s[len(m.group(0)):][:int(m.group(1))] if m else None


fam, unknown = collections.Counter(), []
for s in syms:
    if s in have:
        continue
    c = cls(s)
    if c and c.startswith(FAMILY):
        fam[c] += 1
    else:
        unknown.append(s)
print('undefined: %d; in the kernel lists: %d; from the IOPCIFamily and '
      'IOGraphicsFamily kexts (not checkable here): %d; unknown: %d'
      % (len(syms), len([s for s in syms if s in have]), sum(fam.values()), len(unknown)))
for s in unknown:
    print('  UNKNOWN', s)
sys.exit(1 if unknown else 0)
PY
}

package() {
    [ -d "$out/RadeonNI.kext" ] || { echo "run build first" >&2; exit 1; }
    check
    local rev stage zip
    rev=$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo unknown)
    stage=$out/pkg/RadeonNI-10.5-$(date +%Y%m%d)-$(git -C "$root" rev-parse --short HEAD)
    rm -rf "$out/pkg"
    mkdir -p "$stage"
    cp -R "$out/RadeonNI.kext" "$stage/"
    cp "$root/g5/install.sh" "$root/g5/uninstall.sh" "$root/LICENSE" "$stage/"
    touch "$stage/LEOPARD"
    echo "$rev (10.5 framebuffer-only build)" > "$stage/VERSION"
    cp "$root/g5/LEOPARD-README.txt" "$stage/README.txt"
    [ "${ACCEL:-0}" = 1 ] && cp "$root/g5/LEOPARD-ACCEL-README.txt" "$stage/README.txt"
    if [ "${ACCEL:-0}" = 1 ]; then
        touch "$stage/LEOPARD-ACCEL"
        echo "$rev (10.5 EXPERIMENTAL accelerator build)" > "$stage/VERSION"
        # 2D plug-in: built in the Leopard guest with the 10.4u SDK, as on Tiger
        tar -C "$root" -cf - ga hw | gssh 'rm -rf ~/osx-gpu-ga && mkdir -p ~/osx-gpu-ga && tar -C ~/osx-gpu-ga -xf - && cd ~/osx-gpu-ga/ga && make 2>&1 | tail -2'
        gssh 'tar -C ~/osx-gpu-ga/ga/build -cf - RadeonNIGA.plugin' | tar -C "$stage" -xf -
        [ -f "$stage/RadeonNIGA.plugin/Contents/MacOS/RadeonNIGA" ] || { echo "no 2D plug-in came back" >&2; exit 1; }
        # OpenGL bundle: Mesa inside, from scripts/gld-relink.py (gld/ of this checkout)
        python3 -I "$root/scripts/gld-relink.py" > /dev/null
        mkdir -p "$stage/RadeonNIGLDriver.bundle/Contents/MacOS"
        cp "$root/gld/Info.plist" "$stage/RadeonNIGLDriver.bundle/Contents/Info.plist"
        cp "$root/build/relink/RadeonNIGLDriver.dylib" "$stage/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver"
        chmod 755 "$stage/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver"
        if [ "${LEOPARD_NATIVE:-0}" = 1 ]; then
            tar -C "$root/g5" -cf - activate-qe.m | gssh 'mkdir -p ~/osx-gpu-qe && tar -C ~/osx-gpu-qe -xf - && gcc-4.0 -arch ppc -o ~/osx-gpu-qe/activate-qe ~/osx-gpu-qe/activate-qe.m -framework Cocoa -framework QuartzCore'
            mkdir -p "$stage/ActivateQE.app/Contents/MacOS"
            gssh 'tar -C ~/osx-gpu-qe -cf - activate-qe' | tar -C "$stage/ActivateQE.app/Contents/MacOS" -xf -
            chmod 755 "$stage/ActivateQE.app/Contents/MacOS/activate-qe"
            cp "$root/g5/ActivateQE-Info.plist" "$stage/ActivateQE.app/Contents/Info.plist"
            cp "$root/g5/org.radeonni.activate-qe.plist" "$stage/"
        fi
        "$root/scripts/fetch-firmware.sh" > /dev/null
        cp "$root/firmware/TURKS_pfp.bin" "$root/firmware/TURKS_me.bin" "$root/firmware/TURKS_mc.bin" \
           "$root/firmware/LICENSE.radeon" "$stage/"
    fi
    [ "${ACCEL:-0}" = 1 ] && stage_name=$(basename "$stage")-accel || stage_name=$(basename "$stage")
    zip=$root/build/$stage_name.zip
    rm -f "$zip"
    # The host may have no zip; Python writes it, with the files' modes.
    python3 -I - "$out/pkg" "$(basename "$stage")" "$zip" <<'PY'
import os, sys, zipfile

base, name, out = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for folder, dirs, files in os.walk(os.path.join(base, name)):
        dirs.sort()
        for entry in sorted(dirs) + sorted(files):
            path = os.path.join(folder, entry)
            arc = os.path.relpath(path, base)
            if os.path.isdir(path):
                info = zipfile.ZipInfo.from_file(path, arc + "/")
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, "")
                continue
            info = zipfile.ZipInfo.from_file(path, arc)
            mode = 0o755 if os.access(path, os.X_OK) else 0o644
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as f:
                z.writestr(info, f.read())
PY
    echo "$zip"
    python3 -I -m zipfile -l "$zip" | sed 's/^/  /'
}

case "${1:-}" in
build) build ;;
check) check ;;
package) package ;;
package-accel) ACCEL=1 package ;;
*) sed -n 2,26p "$0"; exit 1 ;;
esac
