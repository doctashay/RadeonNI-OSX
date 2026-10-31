#!/bin/sh
# Build the native manager on Leopard; does not install or change power policy.
set -eu
cd "$(dirname "$0")/.."
app=build/RadeonNI.app
mkdir -p "$app/Contents/MacOS"
/usr/bin/gcc -std=gnu99 -arch ppc -Os -Wall -Ihw tools/guest/gputemp.m \
 -framework Cocoa -framework IOKit -o "$app/Contents/MacOS/RadeonNIManager"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>RadeonNIManager</string>
<key>CFBundleIdentifier</key><string>org.radeonni.manager</string>
<key>CFBundleName</key><string>RadeonNI</string>
<key>CFBundleDisplayName</key><string>RadeonNI Driver Manager</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
</dict></plist>
PLIST
printf '%s\n' "$app"
