#!/bin/sh
# Build the Finder plug-in and per-launch OpenGL selector on Leopard.
set -eu
cd "$(dirname "$0")/.."
cc=${CC:-/usr/bin/gcc-4.2}
out=build/open-on
menu="$out/RadeonNIOpenOn.plugin"
app="$out/OpenOn.app"
mkdir -p "$menu/Contents/MacOS" "$app/Contents/MacOS" "$app/Contents/Frameworks"
"$cc" -std=gnu99 -arch ppc -Os -Wall -bundle -Itools/open-on tools/open-on/menu.c \
 -framework Carbon -framework IOKit -o "$menu/Contents/MacOS/RadeonNIOpenOn"
"$cc" -std=gnu99 -arch ppc -Os -Wall -Itools/open-on tools/open-on/launcher.m \
 -framework Cocoa -framework Carbon -framework IOKit -framework OpenGL -o "$app/Contents/MacOS/OpenOn"
"$cc" -std=gnu99 -arch ppc -arch ppc64 -mmacosx-version-min=10.5 -Os -Wall -dynamiclib \
 tools/open-on/select_gpu.c -framework OpenGL -o "$app/Contents/Frameworks/GPUSelect.dylib"
cat > "$menu/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>RadeonNIOpenOn</string>
<key>CFBundleIdentifier</key><string>org.radeonni.openon.menu</string>
<key>CFBundleName</key><string>RadeonNI Open on</string>
<key>CFBundlePackageType</key><string>BNDL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFPlugInDynamicRegistration</key><false/>
<key>CFPlugInFactories</key><dict><key>E6A8B5D8-5F4A-4BA9-9EA6-D5292DCD49A1</key><string>RadeonNIOpenOnFactory</string></dict>
<key>CFPlugInTypes</key><dict><key>2F6522E9-3E66-11D5-80A7-003065B300BC</key><array><string>E6A8B5D8-5F4A-4BA9-9EA6-D5292DCD49A1</string></array></dict>
</dict></plist>
PLIST
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>OpenOn</string>
<key>CFBundleIdentifier</key><string>org.radeonni.openon.launcher</string>
<key>CFBundleName</key><string>Open on GPU</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSUIElement</key><true/>
</dict></plist>
PLIST
/usr/bin/lipo -info "$app/Contents/Frameworks/GPUSelect.dylib"
printf '%s\n' "$menu" "$app"
