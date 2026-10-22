#!/bin/sh
# Install RadeonNI.kext on a PowerPC Mac running Mac OS X 10.4, so that it
# loads at boot like any other driver.
#
#   sudo ./install.sh [--accel] [--hwcursor] [--check | --try] [vbios.rom]
#
# Run it from the unpacked package directory, on the Mac itself. The driver
# reads the VBIOS from the card's own ROM. A VBIOS image of the card, given
# as an argument or found as vbios.rom next to this script, is stored in the
# kext's Info.plist and used only if the ROM cannot be read; without one the
# script checks that Open Firmware gave the card's ROM an address.
#
# Without options the driver is a plain framebuffer. --accel also starts
# the card's 3D engine at boot, for OpenGL and Quartz Extreme; that needs
# the microcode files TURKS_pfp.bin and TURKS_me.bin next to this script.
# It also installs RadeonNIGLDriver.bundle (OpenGL) and RadeonNIGA.plugin
# from this package into /System/Library/Extensions; they do nothing
# without the accelerator. --hwcursor (with --accel) uses the card's
# hardware cursor; accelerated Leopard installs enable it by default.
# Run the script again without options to go back.
#
# --check only asks the system whether it would load the kext (kextload -t
# -n: dependencies and undefined symbols) and writes nothing. --try loads
# it from /tmp without installing it: a reboot undoes it. Both are for a
# first run on a Mac the driver has not been seen on.
#
# Mac OS X 10.5: only with a package that has the file LEOPARD (see
# scripts/leopard.sh). The framebuffer runs there (2026-10-12). --accel only
# with a package that also has the file LEOPARD-ACCEL: experimental.
#
# Nothing is written to the card. To undo: sudo ./uninstall.sh
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT

set -e

SLE=/System/Library/Extensions
KEXT=RadeonNI.kext
USERLAND="RadeonNIGLDriver.bundle RadeonNIGA.plugin"
here=$(cd "$(dirname "$0")" && pwd)

fail() { echo "install.sh: $*" >&2; exit 1; }

accel=0
hwcursor=0
mode=install
vbios=
[ ! -f "$here/vbios.rom" ] || vbios=$here/vbios.rom
for arg in "$@"; do
    case "$arg" in
    --accel) accel=1 ;;
    --hwcursor) hwcursor=1 ;;
    --check) mode=check ;;
    --try) mode=try ;;
    -*) fail "unknown option $arg" ;;
    *) vbios=$arg
       [ -f "$vbios" ] || fail "VBIOS image not found: $vbios" ;;
    esac
done
[ "$hwcursor" = 0 ] || [ "$accel" = 1 ] || fail "--hwcursor needs --accel"

[ "$(id -u)" = 0 ] || fail "run as root: sudo ./install.sh [vbios.rom]"
[ -d "$here/$KEXT" ] || fail "$KEXT not found next to this script"

case "$(sw_vers -productVersion)" in
10.4*) ;;
10.5*)
    [ -f "$here/LEOPARD" ] ||
        fail "this package is built for Mac OS X 10.4; this is $(sw_vers -productVersion). The 10.5 package has a file LEOPARD."
    if [ "$accel" = 1 ]; then
        # Software cursor restoration can overwrite asynchronous screen updates.
        hwcursor=1
        [ -f "$here/LEOPARD-ACCEL" ] ||
            fail "--accel on 10.5 needs the package built for it (a file LEOPARD-ACCEL, scripts/leopard.sh package-accel)"
        echo "Mac OS X $(sw_vers -productVersion): EXPERIMENTAL accelerator. Tested on a Power Mac G5"
        echo "with 10.5.8 and a Radeon HD 7570; keep a recovery display available."
        echo "If the Mac does not finish booting, see the end of this script's output for the way back."
    else
        echo "Mac OS X $(sw_vers -productVersion): framebuffer-only build."
        echo "Start with: sudo sh ./install.sh --check   (writes nothing)"
    fi ;;
*) fail "this kext is built for Mac OS X 10.4; this is $(sw_vers -productVersion)" ;;
esac
[ "$(uname -p)" = powerpc ] || fail "this kext is PowerPC only"

# A PCI option ROM starts with 55 AA and states its length in 512-byte units.
if [ -n "$vbios" ]; then
    sig=$(od -A n -t x1 -N 3 "$vbios" | tr -d ' \n')
    case "$sig" in
    55aa*) ;;
    *) fail "$vbios does not start with the option ROM signature 55 AA" ;;
    esac
    blocks=$(printf '%d' "0x$(echo "$sig" | cut -c5-6)")
    size=$(wc -c < "$vbios" | tr -d ' ')
    [ "$size" -ge $((blocks * 512)) ] || fail "$vbios is truncated: $size bytes, header says $((blocks * 512))"
    strings "$vbios" | grep -q ATOMBIOS || fail "$vbios has no ATOMBIOS marker"
fi

if [ "$accel" = 1 ]; then
    for f in TURKS_pfp.bin TURKS_me.bin; do
        [ -s "$here/$f" ] || fail "--accel needs the microcode file $f next to this script"
    done
    for b in $USERLAND; do
        [ -d "$here/$b" ] || [ -d "$SLE/$b" ] ||
            fail "--accel needs $b, which is neither in this package nor installed"
    done
fi

# The card in the device tree. Its node may be named after the subsystem
# ID, so it is found by its "compatible" list. Open Firmware lists the
# addresses it assigned, five words for each; the low byte of the first
# word is the register, and 0x30 is the ROM's.
card=$(ioreg -p IODeviceTree -l -w0 2>/dev/null | perl -ne '
    if (/\+-o /) { $compat = $assigned = ""; }
    $compat = $1 if /"compatible" = <(.*)>/;
    $assigned = $1 if /"assigned-addresses" = <([0-9a-f]*)>/;
    if (/^[\s|]*\}/ && $compat =~ /"pci1002,(675d|6758|6759|675b|675f)"/) {
        $found = 1;
        for ($i = 0; $i + 40 <= length($assigned); $i += 40) {
            $rom = 1 if hex(substr($assigned, $i + 6, 2)) == 0x30;
        }
        $compat = "";
    }
    END { print $found ? ($rom ? "rom" : "norom") : "absent"; }
')
case "$card" in
rom)
    echo "Found a Turks card (1002:675d, 6758, 6759, 675b or 675f); its ROM has an address." ;;
norom)
    echo "Found a Turks card (1002:675d, 6758, 6759, 675b or 675f); Open Firmware gave its ROM no address."
    [ -n "$vbios" ] || fail "the driver cannot read the card's ROM on this Mac: give a VBIOS image of the card (sudo ./install.sh /path/to/vbios.rom)" ;;
*)
    echo "Note: no Turks device (1002:675d, 6758, 6759, 675b or 675f) in the device tree right now."
    echo "      The kext will be installed but will only load when that card is present."
    [ -n "$vbios" ] || echo "      Without a VBIOS image it will only start if it can read the card's ROM." ;;
esac
if [ -n "$vbios" ]; then
    echo "VBIOS image $vbios: kept in the kext for when the ROM cannot be read."
else
    echo "No VBIOS image: the driver reads the card's ROM."
fi

tmp=/tmp/RadeonNI-install.$$
rm -rf "$tmp"
mkdir "$tmp"
cp -R "$here/$KEXT" "$tmp/"

# Put the image, if there is one, into the driver's personality as a base64
# <data> property, and the options with it.
perl -MMIME::Base64 -e '
    my ($plist, $rom, $accel, $hwcursor, $dir) = @ARGV;
    local $/;
    open(my $p, "<", $plist) or die "$plist: $!";
    my $text = <$p>;
    close($p);
    $text =~ s{\s*<key>VBIOS</key>\s*<data>.*?</data>}{}s;
    my $extra = "";
    if ($rom ne "") {
        open(my $r, "<", $rom) or die "$rom: $!";
        binmode($r);
        my $data = encode_base64(<$r>, "");
        close($r);
        $extra = "\t\t\t<key>VBIOS</key>\n\t\t\t<data>$data</data>\n";
    }
    if ($accel) {
        # The keys scripts/kext.sh adds for Quartz Extreme under QEMU.
        $extra .= "\t\t\t<key>Accelerator</key>\n\t\t\t<true/>\n";
        for my $fw (["FW_PFP", "TURKS_pfp.bin"], ["FW_ME", "TURKS_me.bin"]) {
            open(my $f, "<", "$dir/$fw->[1]") or die "$fw->[1]: $!";
            binmode($f);
            my $blob = encode_base64(<$f>, "");
            close($f);
            $extra .= "\t\t\t<key>$fw->[0]</key>\n\t\t\t<data>$blob</data>\n";
        }
        # Microcode of the memory controller, if the package has it.
        if (open(my $f, "<", "$dir/TURKS_mc.bin")) {
            binmode($f);
            my $blob = encode_base64(<$f>, "");
            close($f);
            $extra .= "\t\t\t<key>FW_MC</key>\n\t\t\t<data>$blob</data>\n";
        }
        $extra .= "\t\t\t<key>AccelCaps</key>\n\t\t\t<integer>3</integer>\n";
        $extra .= "\t\t\t<key>AGPShim</key>\n\t\t\t<integer>3</integer>\n"
            unless -f "$dir/LEOPARD";
        # Gate unregistered surfaces on Leopard.
        my $gate = -f "$dir/LEOPARD" ? "true" : "false";
        $extra .= "\t\t\t<key>Surfaces</key>\n\t\t\t<true/>\n";
        $extra .= "\t\t\t<key>SurfaceGate</key>\n\t\t\t<$gate/>\n";
        $extra .= "\t\t\t<key>GAPlugin</key>\n\t\t\t<true/>\n";
        $extra .= "\t\t\t<key>HWCursor</key>\n\t\t\t<true/>\n" if $hwcursor;
    }
    $text =~ s{(\t*<key>IOProviderClass</key>)}{$extra$1}
        or die "IOProviderClass not found in $plist\n";
    open($p, ">", $plist) or die "$plist: $!";
    print $p $text;
    close($p);
' "$tmp/$KEXT/Contents/Info.plist" "$vbios" "$accel" "$hwcursor" "$here"

chown -R root:wheel "$tmp/$KEXT"
chmod -R go-w "$tmp/$KEXT"

# Check that the system accepts it before touching the Extensions folder.
kextload -t -n "$tmp/$KEXT" || fail "the system rejects the kext; nothing was installed"
if [ "$mode" = check ]; then
    rm -rf "$tmp"
    echo "The system accepts the kext (kextload -t -n). Nothing was installed."
    exit 0
fi
if [ "$mode" = try ]; then
    kextload -v "$tmp/$KEXT" || fail "the kext did not load (copy kept in $tmp)"
    echo "Loaded from $tmp; not installed. A restart undoes it."
    echo "Messages: grep RadeonNI /var/log/system.log"
    echo "The window server looks for framebuffers when it starts: log out and in."
    exit 0
fi

rm -rf "$SLE/$KEXT"
cp -R "$tmp/$KEXT" "$SLE/"
chown -R root:wheel "$SLE/$KEXT"
chmod -R go-w "$SLE/$KEXT"
rm -rf "$tmp"

# The OpenGL bundle and the 2D plug-in: copied beside the old one and
# renamed into place, so that a running window server keeps its copy.
if [ "$accel" = 1 ]; then
    for b in $USERLAND; do
        [ -d "$here/$b" ] || continue
        rm -rf "$SLE/$b.new" "$SLE/$b.old"
        cp -R "$here/$b" "$SLE/$b.new"
        chown -R root:wheel "$SLE/$b.new"
        chmod -R go-w "$SLE/$b.new"
        [ ! -d "$SLE/$b" ] || mv "$SLE/$b" "$SLE/$b.old"
        mv "$SLE/$b.new" "$SLE/$b"
        rm -rf "$SLE/$b.old"
        echo "Installed $SLE/$b."
    done
    support="/Library/Application Support/RadeonNI"
    mkdir -p "$support"
    if [ -f "$here/QE-AUTOSTART" ] && [ -d "$here/ActivateQE.app" ] &&
       [ -f "$here/org.radeonni.activate-qe.plist" ]; then
        rm -f "$support/nocoreimage"
        mkdir -p /Library/LaunchAgents
        rm -rf "$support/ActivateQE.app"
        cp -R "$here/ActivateQE.app" "$support/"
        cp "$here/org.radeonni.activate-qe.plist" /Library/LaunchAgents/
        chown -R root:wheel "$support/ActivateQE.app"
        chown root:wheel /Library/LaunchAgents/org.radeonni.activate-qe.plist
        chmod -R go-w "$support/ActivateQE.app"
        chmod go-w /Library/LaunchAgents/org.radeonni.activate-qe.plist
        echo "Installed Leopard Quartz Extreme login helper."
    else
        touch "$support/nocoreimage"
        rm -f /Library/LaunchAgents/org.radeonni.activate-qe.plist
        rm -rf "$support/ActivateQE.app"
    fi
else
    rm -f /Library/LaunchAgents/org.radeonni.activate-qe.plist
    rm -rf "/Library/Application Support/RadeonNI/ActivateQE.app"
fi

# Make the system rebuild its extension caches at the next boot.
rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
touch "$SLE"
sync

if [ "$accel" = 1 ]; then
    echo
    echo "Acceleration is ON: the 3D engine starts at boot."
    echo "To go back to the plain framebuffer: sudo ./install.sh"
fi

cat <<'MSG'

Installed /System/Library/Extensions/RadeonNI.kext.
Restart the Mac. The driver starts at boot when it finds the card; its
messages, with where it took the VBIOS from, are in the system log:
  grep RadeonNI /var/log/system.log

If the Mac does not finish booting with the driver installed:
  - hold Shift while it starts (Safe Boot does not load third-party
    drivers), log in and run:  sudo ./uninstall.sh
  - or hold Command-S for single-user mode and type:
        /sbin/fsck -fy && /sbin/mount -uw /
        rm -rf /System/Library/Extensions/RadeonNI.kext
        rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
        reboot
  - or take the card out: without it the driver is never loaded.
MSG
