#!/usr/bin/env bash
# Build the 10.5 PowerPC GL bundle with the Darwin 9 cross toolchain.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
parent=$(dirname "$root")
src=$root/third_party/mesa-26.2.4
image=${RDN_DARWIN9_IMAGE:-darwin-legacy-cross:local}
pydeps=${RDN_PYDEPS:-$parent/pydeps}
sdk=/opt/darwin-legacy-cross/MacOSX-SDKs/SDK/MacOSX10.5.sdk
compat=$root/third_party/darwin8-compat

[ -d "$src" ] || { echo "extract Mesa 26.2.4 into $src" >&2; exit 1; }
[ -d "$pydeps/mesonbuild" ] || { echo "Meson Python modules are missing in $pydeps" >&2; exit 1; }

cd "$src"
for p in "$root"/mesa/patches/*.patch; do
    if patch -p1 -R --dry-run -s -f < "$p" > /dev/null 2>&1; then
        continue
    fi
    patch -p1 --batch --forward < "$p"
done
for d in src/gallium/winsys/rdn src/gallium/frontends/rdn src/gallium/targets/rdn; do
    if [ -L "$d" ]; then rm "$d"; fi
done
rsync -a --delete "$root/mesa/winsys/" src/gallium/winsys/rdn/
rsync -a --delete "$root/mesa/frontend/" src/gallium/frontends/rdn/
rsync -a --delete --exclude hw --exclude tests --exclude gld "$root/mesa/target/" src/gallium/targets/rdn/
rsync -a --delete "$root/hw/" src/gallium/targets/rdn/hw/
rsync -a --delete "$root/mesa/tests/" src/gallium/targets/rdn/tests/
rsync -a --delete --exclude build "$root/gld/" src/gallium/targets/rdn/gld/
sed -i "s@/usr/local/MacOSX10.4u.sdk@$sdk@g" src/gallium/targets/rdn/meson.build

mkdir -p "$compat"
rsync -a --delete "$root/mesa/darwin8/" "$compat/src/"
cat > "$root/third_party/mesa-darwin9-cross.ini" <<INI
[binaries]
c = '/opt/darwin-legacy-cross/ppc-darwin9-gcc14-objc/bin/powerpc-apple-darwin9-gcc'
cpp = '/opt/darwin-legacy-cross/ppc-darwin9-gcc14-objc/bin/powerpc-apple-darwin9-g++'
objc = '/opt/darwin-legacy-cross/ppc-darwin9-gcc14-objc/bin/powerpc-apple-darwin9-gcc'
ar = '/opt/darwin-legacy-cross/linux-ppc-darwin9-xtools/bin/powerpc-apple-darwin9-ar'
strip = '/opt/darwin-legacy-cross/linux-ppc-darwin9-xtools/bin/powerpc-apple-darwin9-strip'
ranlib = '/opt/darwin-legacy-cross/linux-ppc-darwin9-xtools/bin/powerpc-apple-darwin9-ranlib'
pkg-config = 'false'

[built-in options]
c_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-mlongcall', '-mcpu=970', '-mno-powerpc64', '-fno-strict-aliasing', '-isystem', '$compat/src/include']
cpp_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-mlongcall', '-mcpu=970', '-mno-powerpc64', '-fno-strict-aliasing', '-isystem', '$compat/src/include', '-include', '$compat/src/tiger_compat.h']
objc_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-mlongcall', '-mcpu=970', '-mno-powerpc64', '-fno-strict-aliasing', '-isystem', '$compat/src/include']
c_link_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-L/opt/darwin-legacy-cross/deps/libatomic-ppc/lib', '-static-libgcc', '-Wl,-dead_strip', '$compat/libtigercompat.a']
cpp_link_args = ['-isysroot', '$sdk', '-mmacosx-version-min=10.5', '-L/opt/darwin-legacy-cross/deps/libatomic-ppc/lib', '-static-libgcc', '-static-libstdc++', '-Wl,-dead_strip', '$compat/tiger_emutls.o', '$compat/libtigercompat.a']

[host_machine]
system = 'darwin'
cpu_family = 'ppc'
cpu = 'ppc7400'
endian = 'big'
INI

docker run --rm --network none \
    -v "$parent:$parent" -w "$src" -e PYTHONPATH="$pydeps" -e RDN_ROOT="$root" \
    "$image" bash -lc '
      set -e
      . /opt/darwin-legacy-cross/bin/darwin-legacy-cross-env
      cc=powerpc-apple-darwin9-gcc
      compat=$RDN_ROOT/third_party/darwin8-compat
      sdk=$DARWIN_SDK_ROOT
      "$cc" -isysroot "$sdk" -O2 -Wall -mmacosx-version-min=10.5 -c -o "$compat/tiger_compat.o" "$compat/src/tiger_compat.c"
      "$cc" -isysroot "$sdk" -O2 -Wall -mmacosx-version-min=10.5 -c -o "$compat/tiger_emutls.o" "$compat/src/tiger_emutls.c"
      powerpc-apple-darwin9-ar rcs "$compat/libtigercompat.a" "$compat/tiger_compat.o"
      if [ ! -f build-darwin9/build.ninja ]; then
        python3 -m mesonbuild.mesonmain setup build-darwin9 \
          -Drdn=true -Dgallium-drivers=r600,softpipe -Dvulkan-drivers= \
          -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
          -Dplatforms= -Dgles1=disabled -Dgles2=disabled -Dzstd=disabled \
          -Dvalgrind=disabled -Dlibunwind=disabled -Dvideo-codecs= \
          -Dgallium-va=disabled -Dbuildtype=release -Doptimization=2 \
          -Dxmlconfig=disabled -Dshader-cache=disabled -Db_ndebug=true \
          --cross-file "$RDN_ROOT/third_party/mesa-darwin9-cross.ini" \
          --wrap-mode=nodownload
      fi
      python3 "$RDN_ROOT/scripts/meson-darwin9-args.py" \
        "$RDN_ROOT/third_party/mesa-darwin9-cross.ini" build-darwin9
      ninja -C build-darwin9 src/gallium/targets/rdn/RadeonNIGLDriver.dylib
    '
