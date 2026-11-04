# Leopard/Cayman build used for the GTA V port

Read this before rebuilding or installing the GL driver for the V7900 (Cayman, `1002:6704`) on the
Leopard G5, or when asking which source produced the installed binary.

## Where the installed GL driver comes from

- Mesa patches for this work live in `mesa/patches/` here (0099 to 0118; 0108, 0113 to 0115, 0117
  and 0119 were rejected and are not in this directory).
- The binary is not built from a clean apply of those patches. It is built from the already-patched
  tree `F:/radeonni-leopard-build/reconcile-delivery-20261022/third_party/mesa-26.2.4/` in
  `build-darwin9/` (PPC32) and the matching PPC64 build directory, with incremental compile/link
  commands (`C:/Users/shabe/outputs/rage5-driver-build-incremental.sh` and the
  `rage5-driver-build-commands*.txt` files next to it).
- That tree's sources contain the changes from these patches (checked for 0118 in
  `src/gallium/drivers/r600/r600_query.c`). Reproducing the binary from git therefore means applying
  `mesa/patches/` in order to the official 26.2.4 archive and building both slices.
- Installed FAT GLD1 driver at the time of this checkpoint: MD5 `a651ae44b9ca53c0e03ebc65064c8178`
  (PPC32 runtime-validated; PPC64 build/link validated only). Rollback copies are under
  `/Users/shay/rage5-port-tests/20261029-rdn-performance` on the G5.

## Installed shared-uniform fix

The current GL modules include `mesa/patches/0122-shared-uniform-context-flush.patch`.
The patch and native regression test live in this worktree. Build inputs were taken
from the prepared Mesa tree above; the uniform and buffer-placement objects were
recompiled for both architectures. PPC32 linking also needs `idep_xmlconfig`, now
included in `mesa/target/meson.build`.

Build commands, universal manifests and installation results are in
`C:/Users/shabe/outputs/rdn-shared-uniform-20261101/`. Native shared-opacity tests
pass on both GPUs in PPC32/PPC64. The driver fix alone did not restore Aurora's animated changelog. The browser's
What's New CSS now uses backwards fill to release finished transformed layers;
that page was visually checked during and after the animation. This is not a
general retained-layer compositor fix. The original four binaries are preserved at
`/Users/shay/RadeonNI-diagnostics/shared-uniform-20261101/before` on the G5.
WindowServer and the running game were not restarted.

## Direct constant uploads and command-buffer reuse

The V7900's GLD1 module now includes patch 0123 and the bounded retired-IB pool
in `mesa/winsys/`. Big-endian constants are converted directly into the mapped
upload allocation. Dedicated command allocations are reused only after their
fences complete, with an eight-buffer/2 MB idle limit per winsys. The four-pending-IB
throttle and failed-submission quarantine remain intact. Presentation is unchanged.

Build script, source snapshots, manifests and native logs:
`C:/Users/shabe/outputs/rdn-upload-ib-pool-20261101/`.
`build.py` takes optional `32` or `64`; `package.py` verifies no new undefined
symbols and creates the universal module. It uses the prepared F: Mesa tree and
the prior shared-uniform package's `libmesa.a` and `libr600.a`, replacing only the
changed r600 and winsys objects. PPC64 needs the compiler under
`/mnt/chromium-ppc/ppc64-darwin/toolchain`; its image is
`I:/WSLDisks/ChromiumPPC/ext4.img`. The F: build-directory dylibs are not this package.

Installed universal GLD1 SHA256:
`25ee57678c511c312515435ac4692345f7b97da2c3016158129663d8deb52cd7`.
Only GLD1 was replaced. Rollback:
`/Users/shay/RadeonNI-diagnostics/upload-ib-pool-20261101/before/RadeonNIGLDriver1`.
WindowServer was not restarted; GTA was relaunched to load the new module.

Validation: `python3 tests/ib_pool.py <prepared-mesa-tree>` passes with ASan/UBSan.
Native `tests/upload_submission.c` passes 32,768 pixel-channel checks across 256
flushed draws in PPC32/PPC64, with `RDN_GLTHREAD=0` and `1`. Each run submitted 289
IBs with four kernel allocations and 285 pool hits. The shared-uniform test also
passes all 63 opacity checks in both architectures and modes. These are correctness
and allocation-count results, not a game FPS comparison.

GTA entered the city on the new module; `city.png` and `live.sample` record the
visual check and driving sample. Constant-buffer setup accounted for 0.96% of
render-thread samples (previously 2.66%), and IB allocation 0.35% (previously
0.76%). Overall submission was 3.32% versus 2.83%, and presentation 13.15% versus
8.32%; the scenes differ, so these percentages do not establish a net FPS gain.
The CGS binding query is inside Apple's flush operation. Bypassing that operation
would also bypass WindowServer's covered-window composition decisions; this build
does not change that path.

## Rules

- Patch 0119 (all DEFAULT buffers in hidden VRAM) broke WindowServer and must not be installed.
- Driver test history and evidence for this work are in `C:/Users/shabe/outputs/rage5-driver-assessment.txt`.
