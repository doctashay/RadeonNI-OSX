#!/bin/sh
# From Leopard: put the UVD-capable kext binary (built in the Tiger guest) and the UVD firmware into the RadeonNI.kext on
# the Tiger volume of this G5, with RelocateVRAM on, keeping the old files next to it. Run with sudo, then bless the Tiger
# volume (`bless --mount /Volumes/Tiger --setBoot`) and restart.
set -e
K=/Volumes/Tiger/System/Library/Extensions/RadeonNI.kext
D=$(cd "$(dirname "$0")" && pwd)
S=$(date +%Y%m%d%H%M)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -d "$K" ] || { echo "no $K"; exit 1; }
cp "$K/Contents/MacOS/RadeonNI" "$K/Contents/RadeonNI.before-uvd-$S"
cp "$K/Contents/Info.plist" "$K/Contents/Info.plist.before-uvd-$S"
cp "$D/RadeonNI.tiger" "$K/Contents/MacOS/RadeonNI"
perl -e '
use MIME::Base64;
my ($plist, $fw) = @ARGV;
local $/;
open(F, "<", $plist) or die "$plist: $!"; my $t = <F>; close F;
open(U, "<", $fw) or die "$fw: $!"; binmode U; my $b = encode_base64(<U>, ""); close U;
$t =~ s{\s*<key>FW_UVD</key>\s*<data>.*?</data>}{}s;
$t =~ s{\s*<key>RelocateVRAM</key>\s*<true/>}{}s;
$t =~ s{(<key>FW_ME</key>)}{<key>FW_UVD</key>\n\t\t\t<data>$b</data>\n\t\t\t$1} or die "no FW_ME in the personality";
$t =~ s{(<key>Accelerator</key>)}{<key>RelocateVRAM</key>\n\t\t\t<true/>\n\t\t\t$1} or die "no Accelerator in the personality";
open(O, ">", $plist) or die; print O $t; close O;
print "personality: FW_UVD and RelocateVRAM added\n";' "$K/Contents/Info.plist" "$D/SUMO_uvd.bin"
plutil -lint "$K/Contents/Info.plist"
chown -R root:wheel "$K"
chmod -R go-w "$K"
touch /Volumes/Tiger/System/Library/Extensions
echo "done; backups: RadeonNI.before-uvd-$S, Info.plist.before-uvd-$S"
