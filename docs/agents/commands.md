# Commands and tools

Moved out of CLAUDE.md. Read before running a script, the guest, QEMU, a
build or an install. For reaching the G5 or the guest use the `test-macs`
skill (`scripts/mac.sh`).

- `make`: builds the hardware library and tests for x86 (`build/x86/`) and
  big-endian PowerPC (`build/ppc/`).
- `make test`: runs each test natively and under `qemu-ppc`; the two outputs
  must be identical. Needs `private/vbios.rom` and the reference trace phase
  files, and skips loudly without them. A single test by hand:
  `build/x86/atom_replay private/vbios.rom traces/ref-radeon-2.phase-a1.txt`.
- `scripts/fetch-deps.sh`: PowerPC cross toolchain and Linux radeon sources
  into `third_party/`. Run once, with `scripts/build-qemu.sh`, on a fresh
  checkout.
- `sudo build/x86/rdn_tool accel` (after `post` and `modeset`): start the
  3D engine and command processor and draw the self-test picture.
  `sudo build/x86/rdn_tool grab file.ppm` saves the scanout surface, so the
  picture can be checked without the monitor (`pnmtopng` to view it).
  The microcode must be in `firmware/` (`zstd -d` from `/lib/firmware/radeon`).
- `sudo build/x86/rdn_tool {status|post|vramtest|edid|modeset|peek}`: our code on the real
  card through sysfs; `-t file` logs accesses in the trace format, `-n`
  avoids the I/O BAR. Needs `scripts/card-bind.sh none` first.
- `scripts/card-reset.sh`: back to the un-POSTed state (bus reset).
- `scripts/card-quiet.sh {apply|status}`: stop the card and its root port
  from escalating PCIe errors (see the host safety rules).
- `sudo scripts/card-state.py`: is the card POSTed (read-only).
- `scripts/host-inventory.sh`: read-only host inspection (GPUs, drivers,
  IOMMU groups, tools). Re-run after any hardware or kernel change.
- `scripts/card-bind.sh {status|vfio|none|radeon}`: move the 7570 between
  host drivers at runtime. After a host reboot it is driverless (`radeon` is
  blacklisted) and un-POSTed.
- `scripts/x86-trace-guest.sh <name>` then `scripts/trace-split.py
  traces/<name>.log`: capture and reduce a reference trace of the stock
  Linux driver. The compact per-phase files are diffable.
- `scripts/build-qemu.sh`: builds QEMU 11.1.1 (`qemu-system-ppc`, `qemu-ppc`)
  into `third_party/qemu/`, applying `patches/qemu/*.patch`. The host has no
  packaged PowerPC QEMU; always use this build.
- `scripts/guest-ctl.py {shot|click|key|type}` and `scripts/guest-wait.sh`:
  operate the guest's GUI through QMP (screenshots to `build/shot.png`,
  clicks at screenshot coordinates). Look at the screenshot before clicking.
- `scripts/tiger.sh {create|install|cdrom|run|passthru|snapshot|restore|ssh}`:
  Tiger guest lifecycle. Screen on VNC `127.0.0.1:5901`, ssh on port 2222.
  `passthru <addr>` attaches the card; it needs sudo, and so does
  `guest-ctl.py` while that guest runs. With `TIGER_TRACE=1` it adds
  `x-no-mmap=on` and writes the register trace to `traces/`, which makes
  framebuffer drawing extremely slow.

- `scripts/leopard-guest.sh {run|cdrom|snapshot|restore|ssh}`: the Mac OS X
  10.5.9 (Sorbet) guest beside Tiger: `images/leopard.qcow2`, ssh 2223,
  VNC :2, sockets `build/leopard-*.sock`. `scripts/tiger.sh` reads
  `TIGER_SSH_PORT`, `TIGER_VNC`, `TIGER_SOCK_PREFIX`, `TIGER_SERIAL_LOG`,
  `TIGER_EXTRA_DISK`; `guest-ctl.py` reads `GUEST_QMP`, `GUEST_SHOT`.
  `LEOPARD_NATIVE=1 scripts/leopard.sh build|check` builds the kext in it.
  How it was made: `docs/LEOPARD.md`.
- `TIGER_NOVGA=1` with `scripts/tiger.sh passthru`: no emulated display,
  so that the 7570 is the only one (works, 2026-10-05; nothing on VNC,
  wait for ssh rather than `guest-cycle.sh ready`, then `kext.sh up`).
  The default keeps the emulated display. `~/gl/cgmode main N` in the
  guest makes a display the main one; the 7570's is, since 2026-10-05.
- `TIGER_EVDEV=/dev/input/by-id/...` with `scripts/tiger.sh` and
  `scripts/guest-cycle.sh ready`, or `scripts/tiger.sh evdev [node|off]`
  on a running guest: a real keyboard and pointer for the guest through
  QEMU's `input-linux` (works, 2026-10-05). The user's is
  `usb-Logitech_USB_Receiver-if02-event-mouse`. Not `usb-host`: it stalls
  the guest's USB bus (journal, 2026-10-05). Run `evdev` again after a
  guest restart or after replugging the device.
- `tools/guest/cgmode.c`: build in the guest (`gcc -o cgmode cgmode.c
  -framework ApplicationServices`) to list and switch display modes through
  Quartz and to put the cursor on a display.
- `scripts/kext.sh up`: build `kext/RadeonNI` inside the running guest, load
  it from `/tmp` with `kextload`, and restart the guest's window server so
  that it uses the screen. The steps are also available one by one
  (`build`, `load`, `activate`), plus `unload`, `log` and `uninstall`.
- `RDN_ACCEL=1 scripts/kext.sh up`: as above, and the kext also publishes
  its accelerator service (`kext/RadeonNI/RadeonNIAccel.cpp`), which makes
  OpenGL load the GL bundle. Without the variable the kext is phase 1's.
- `scripts/gld.sh install-mesa`: install the host-built driver bundle that
  has Mesa inside (`scripts/build-mesa.sh darwin
  src/gallium/targets/rdn/RadeonNIGLDriver.dylib`). `RDN_GLD_LOG=file` in a
  program's environment makes it log.
- `tools/guest/glwin.c`: a windowed GLUT test for the guest.
- `tools/guest/fences.c`: how many command buffers the GPU completed in a
  few seconds; zero when nothing draws with the card.
- For Quartz Extreme the user chose trial and error in the guest
  (2026-10-05), not captures from the real G5.
- `scripts/gld.sh {build|install|uninstall|log|clearlog}`: build
  `gld/RadeonNIGLDriver.bundle` in the guest and install it in the guest's
  `/System/Library/Extensions` (OpenGL loads it only from there). `log`
  shows every `gld*` call made to it (`/tmp/rdngld.log` in the guest).
- `RDN_HWCURSOR=1` with `scripts/kext.sh up`: the card's hardware cursor
  (`hw/rdn_cursor.c`) instead of `IOFramebuffer` drawing the cursor with
  the CPU. Confirmed on the monitor by the user (2026-10-05) with
  `~/gl/curmove` (`tools/guest/curmove.c`), which moves the pointer over a
  display for a given time.
- `scripts/ga.sh {build|install|uninstall}`: the 2D accelerator plug-in
  (`ga/RadeonNIGA.plugin`) the window server wants before it tries Quartz
  Extreme; built and installed in the guest like the GL bundle.
- In the guest, `touch /tmp/rdngld.on` makes every process that loads the
  bundle log its `gld*` calls to `/tmp/rdngld.<pid>.log`; with
  `/tmp/rdngld.trace` as well, every GL call. `~/gl/rdnuc grab file.ppm`
  saves the 7570's screen, `~/gl/qe` says whether Quartz Extreme is in
  use, `~/gl/fences` counts GPU command buffers over 5 s
  (`tools/guest/`).
- `tools/guest/glprobe.c`: build in the guest (command in its header) to
  list OpenGL renderers and to draw off-screen on a chosen renderer
  (`glprobe draw 0x20400` goes through our bundle).
- `scripts/build-mesa.sh`: fetch Mesa 26.2.4 into `third_party/`, apply
  `mesa/patches/`, copy `mesa/` and `hw/` into its tree and build
  `librdngl.so` for the host. Run it again after editing `mesa/` or `hw/`.
  Then (card posted and mode set with `rdn_tool`):
  `sudo third_party/mesa-26.2.4/build-x86/src/gallium/targets/rdn/rdn_gltest
  -n 90 -s 1366 768 1408 -o build/gltest.ppm`. `scripts/build-mesa.sh ppc`
  builds the same test static for big-endian PowerPC; run it with
  `sudo third_party/qemu/qemu-ppc -cpu 7447a .../build-ppc/.../rdn_gltest`.
  `-a`, `-b` and `-D` vary the scene so that runs can be told apart.
- `scripts/darwin.sh image` once, then `scripts/darwin.sh <command>`: run
  the cross toolchain for Tiger (GCC 14.2 for `powerpc-apple-darwin8`, the
  10.4u SDK) in a container that sees only the repository.
  `scripts/build-mesa.sh darwin [targets]` builds Mesa with it, at -O2
  (`RDN_MESA_OPT=s`: for size, into `build-darwin-Os`);
  `.../build-darwin/src/gallium/targets/rdn/rdn_gltest` is the test program
  to copy into the guest (`-s` draws on the screen).
- Video decode (read `docs/UVD.md`): `scripts/build-mesa.sh darwin
  src/gallium/targets/rdn/rdn_uvdtest src/gallium/targets/rdn/librdnvd.dylib` builds the test and the library;
  `scripts/uvd-testvec.sh <dir> <name>` makes a vector (x264 stream and ffmpeg's decode); `rdn_uvdtest
  stream.h264 reference.yuv` on the G5 compares every picture; `scripts/uvd-parse-check.py <h264dump> <dir>` checks
  the H.264 layer's output order against ffmpeg on the host (`gcc -o h264dump video/tests/h264dump.c
  video/rdnvd_h264.c`); `g5/install-kext-binary.sh` replaces only the kext binary on the G5, keeping the old one.
- `scripts/guest-cycle.sh down`, then `sudo scripts/tiger.sh passthru
  0000:10:00.0` in the background, then `scripts/guest-cycle.sh ready`: a
  clean guest restart (a loaded framebuffer kext cannot be unloaded) with
  the card's error masking re-applied. `TIGER_BOOTARGS=debug=0x100` makes
  a guest kernel panic print its backtrace on the emulated screen.
- `tools/guest/qe.c`: is Quartz Extreme in use on each display.
- `scripts/make-g5-package.sh [--with-vbios]`: package the guest-built
  kext and 2D plug-in, the host-built OpenGL bundle, the microcode with
  its licence (`scripts/fetch-firmware.sh` gets it from the host's
  linux-firmware or kernel.org when `firmware/` lacks it) and
  `g5/install.sh`, `g5/uninstall.sh`, `g5/README.txt` into
  `build/RadeonNI-g5.tar.gz`, to be unpacked and installed on the real
  Mac. There, `sudo ./install.sh --accel [--hwcursor]` installs the kext
  with the Quartz Extreme personality `kext.sh` uses under QEMU, plus the
  bundle and the plug-in; without options, phase 1's. The installer's
  bundle and plug-in step has not run anywhere yet.
- `scripts/build-piglit.sh`, `tools/piglit/run.py` (stage, start, fetch,
  summary) and `docs/PIGLIT.md`: piglit's GL 2.1 tests built for Tiger and
  run on the G5 (2026-10-08). `run.py start` must stay in the foreground of
  its ssh session (run it as a background job), else the tests abort.
- `scripts/make-mpkg.sh [stage [out]]` (2026-10-09; `make-dist.sh` runs it, `--no-mpkg`
  skips): `RadeonNI.mpkg` for Installer.app, sources in `g5/installer/`: required
  RadeonNI package, optional unselected Open Firmware package (its postflight runs
  `of-install.sh --yes`). Archives made by the guest's `mkbom`/`pax`. Opened in the guest's
  Installer, and run on the G5 by the user (2026-10-09, "worked great"; journal).
- `scripts/make-dist.sh [--keep-gl]`: the archive for other people,
  `build/RadeonNI-<date>-<commit>.zip`: what `make-g5-package.sh` builds
  plus `README.md`, `INSTALL.txt` (`g5/README.txt`) and the licences
  (ours, the microcode's, Mesa's), never a VBIOS. `--keep-gl` skips the
  Mesa build and packs the bundle as last built. Unpacked and installed
  from on the G5 (2026-10-06); needs nothing but Tiger's base install
  there. Never tried on a Mac without Xcode or on any other Mac.
- `rdn_rom=0` as a boot argument (`sudo nvram boot-args="rdn_rom=0"`):
  the kext leaves the card's ROM alone and uses the VBIOS image in its
  personality; it does not start if `install.sh` was given none.
- `kext/RomProbe`: a kext to load by hand on a running Mac (`make` on
  Tiger, `kextload` from a root-owned copy, `kextunload -b
  org.osxgpu.driver.RadeonNIRomProbe`). It reads the card's whole ROM BAR
  and publishes it as the property `ROM` of its registry entry (`ioreg -c
  RadeonNIRomProbe -l -w0`), without touching the driver.

## Releases: one for Tiger, one for Leopard (2026-10-15)

Both come from the same tree (`main`); the OS difference is in the package, in
`gld/rdn_engine.h` (the engine's layout by kernel major version, chosen at run time) and in
the kext's personality written by `g5/install.sh` (`Surfaces`, `SurfaceGate`).

| | Tiger 10.4.11 | Leopard 10.5.9 |
|---|---|---|
| Command | `scripts/make-dist.sh [--keep-gl] [--no-mpkg]` | `LEOPARD_NATIVE=1 scripts/leopard.sh build && LEOPARD_NATIVE=1 scripts/leopard.sh package-accel` |
| Needs running | Tiger guest (`scripts/tiger.sh run`): kext and 2D plug-in with Xcode 2.5 | Leopard guest (`scripts/leopard-guest.sh run`): kext and plug-in with Xcode 3.1.4 |
| OpenGL bundle | Mesa cross-built on the host (`scripts/build-mesa.sh darwin`, in the checkout that owns `third_party/mesa-*`) | the same tree's objects, only `gld/*.c` recompiled and relinked with the export list (`scripts/gld-relink.py`; no Mesa rebuild) |
| Output | `build/RadeonNI-<date>-<commit>.zip` (+ `RadeonNI.mpkg`) | `build/RadeonNI-10.5-<date>-<commit>-accel.zip` (`LEOPARD`, `LEOPARD-ACCEL` markers) |
| Install | `sudo ./install.sh --accel` or the mpkg | `sudo ./install.sh --check`, then `sudo ./install.sh --accel`; restart |
| README | `g5/README.txt` | `g5/LEOPARD-ACCEL-README.txt` (`g5/LEOPARD-README.txt` for the framebuffer-only `package`) |

`install.sh` refuses a 10.4 package on 10.5 (no `LEOPARD`) and `--accel` on 10.5 without
`LEOPARD-ACCEL`. Commit before packaging: the file name carries the HEAD commit, and
`make-dist.sh` adds `-dirty`. Between a Leopard bundle and the Tiger one the difference is
only which Mesa objects were last built; both export the same 66 `gld*` names
(`scripts/gld-names-check.sh tiger|leopard <dylib>`).
