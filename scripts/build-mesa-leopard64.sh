#!/usr/bin/env bash
# Build the PPC64 slice using the Chromium PPC64 Darwin toolchain.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
parent=$(dirname "$root")
src=$root/third_party/mesa-26.2.4
# The PPC64 build uses the Mesa tree prepared by the PPC32 build.
for feature_patch in "$root"/mesa/patches/010[234567]-*.patch "$root"/mesa/patches/0112-*.patch "$root"/mesa/patches/011[68]-*.patch; do
    if ! patch -d "$src" -p1 -R --dry-run -s -f < "$feature_patch" >/dev/null 2>&1; then
        patch -d "$src" -p1 --batch --forward < "$feature_patch"
    fi
done

tc=${RDN_PPC64_TOOLCHAIN:-/mnt/chromium-ppc/ppc64-darwin/toolchain}
sdk=${RDN_PPC64_SDK:-/home/shay/toolchains/osxcross/target/SDK/MacOSX10.5.sdk}
compat=$root/third_party/darwin9-compat64
export PYTHONPATH=${RDN_PYDEPS:-$parent/pydeps}
export PATH="$tc/bin:$PATH"
mkdir -p "$compat"
rsync -ac --no-times "$root/mesa/darwin8/" "$compat/src/"
rsync -ac --no-times "$root/mesa/winsys/" "$src/src/gallium/winsys/rdn/"
rsync -ac --no-times "$root/mesa/frontend/" "$src/src/gallium/frontends/rdn/"
rsync -ac --no-times --exclude hw --exclude tests --exclude gld --exclude video "$root/mesa/target/" "$src/src/gallium/targets/rdn/"
rsync -ac --no-times --delete --exclude tests "$root/video/" "$src/src/gallium/targets/rdn/video/"
rsync -ac --no-times "$root/hw/" "$src/src/gallium/targets/rdn/hw/"
rsync -ac --no-times "$root/mesa/tests/" "$src/src/gallium/targets/rdn/tests/"
rsync -ac --no-times --exclude build "$root/gld/" "$src/src/gallium/targets/rdn/gld/"
cc=$tc/bin/powerpc64-apple-darwin9-gcc
"$cc" -isysroot "$sdk" -O2 -mmacosx-version-min=10.5 -c "$compat/src/tiger_compat.c" -o "$compat/tiger_compat.o"
"$cc" -isysroot "$sdk" -O2 -mmacosx-version-min=10.5 -c "$compat/src/tiger_emutls.c" -o "$compat/tiger_emutls.o"
powerpc64-apple-darwin9-ar rcs "$compat/libtigercompat.a" "$compat/tiger_compat.o"
cross=$root/third_party/mesa-darwin9-ppc64-cross.ini
cat > "$cross.tmp" <<INI
[binaries]
c = '$cc'
cpp = '$tc/bin/powerpc64-apple-darwin9-g++'
objc = '$cc'
ar = '$tc/bin/powerpc64-apple-darwin9-ar'
strip = '$tc/bin/powerpc64-apple-darwin9-strip'
ranlib = '$tc/bin/powerpc64-apple-darwin9-ranlib'
pkg-config = 'false'
[properties]
rdn_sdk = '$sdk'
[built-in options]
c_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-mcpu=970', '-mlongcall', '-fno-strict-aliasing', '-isystem', '$compat/src/include']
cpp_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-mcpu=970', '-mlongcall', '-fno-strict-aliasing', '-isystem', '$compat/src/include', '-include', '$compat/src/tiger_compat.h']
objc_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5']
c_link_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-static-libgcc', '$compat/libtigercompat.a']
cpp_link_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-static-libgcc', '-static-libstdc++', '$compat/tiger_emutls.o', '$compat/libtigercompat.a']
[host_machine]
system = 'darwin'
cpu_family = 'ppc64'
cpu = '970'
endian = 'big'
INI
if cmp -s "$cross.tmp" "$cross"; then
    rm "$cross.tmp"
else
    mv "$cross.tmp" "$cross"
fi
cd "$src"
if [ ! -f build-darwin9-ppc64/build.ninja ]; then
    python3 -m mesonbuild.mesonmain setup build-darwin9-ppc64 \
      -Drdn=true -Dgallium-drivers=r600,softpipe -Dvulkan-drivers= \
      -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
      -Dplatforms= -Dgles1=disabled -Dgles2=disabled -Dzstd=disabled \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dvideo-codecs= \
      -Dgallium-va=disabled -Dbuildtype=release -Doptimization=2 \
      -Dxmlconfig=disabled -Dshader-cache=enabled -Dshader-cache-max-size=64M -Db_ndebug=true \
      --cross-file "$root/third_party/mesa-darwin9-ppc64-cross.ini" --wrap-mode=nodownload
fi
python3 -m mesonbuild.mesonmain configure build-darwin9-ppc64 -Dshader-cache=enabled -Dshader-cache-max-size=64M
ninja -j "${RDN_JOBS:-8}" -C build-darwin9-ppc64 src/gallium/targets/rdn/RadeonNIGLDriver.dylib
slice=$src/build-darwin9-ppc64/src/gallium/targets/rdn/RadeonNIGLDriver.dylib
ppc32_dir=${RDN_PPC32_DIR:-}
if [ -z "$ppc32_dir" ] && [ -n "${RDN_PPC32_SLICE:-}" ]; then
    ppc32_dir=$(dirname "$RDN_PPC32_SLICE")
fi
if [ -n "$ppc32_dir" ]; then
    output=${RDN_UNIVERSAL_OUTPUT:-$root/gld/build-leopard-universal/RadeonNIGLDriver}
    [ "$(basename "$output")" = RadeonNIGLDriver ] || {
        echo "universal output must end in RadeonNIGLDriver; all four modules are packaged together" >&2
        exit 1
    }
    python3 "$root/scripts/package-leopard-gld.py" "$(dirname "$output")" \
        --ppc32 "$ppc32_dir" --ppc64 "$(dirname "$slice")"
else
    echo "PPC64 slices built only; set RDN_PPC32_DIR to package installable universal GLDs" >&2
fi
