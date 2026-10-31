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

## Rules

- Patch 0119 (all DEFAULT buffers in hidden VRAM) broke WindowServer and must not be installed.
- Driver test history and evidence for this work are in `C:/Users/shabe/outputs/rage5-driver-assessment.txt`.
