# Mac OS X 10.5 (Leopard, PowerPC): framebuffer kext and GL bundle study

Read this before touching 10.5 support. The user reports that the
hand-patched kext worked on a 10.5 Mac; the sections below that predate it
(paper checks, 2026-10-11) say "never run" and are kept as the record of
what was compared. Since 2026-10-11 there is also a **Leopard guest** (Sorbet
Leopard R15 = 10.5.9, Xcode 3.1.4) in which the kext is built natively and
the GL bundle can be inspected (next section). The early GL bundle result is recorded under "GL bundle on 10.5".
The native G5 backing-surface path below supersedes that result.

Build and package: `scripts/leopard.sh build|check|package`. The kext is
built in the Tiger guest with `make LEOPARD=1` (its own `build-leopard/`),
which adds `kext/RadeonNI/leopard.s`. `g5/install.sh` accepts 10.5 only in
a package with the file `LEOPARD` and without `--accel`; `--check`
(`kextload -t -n`, writes nothing) and `--try` (load from /tmp, not
installed) are for a first run.

## Native G5 backing surfaces (2026-10-22)

Packed AppKit backings are pinned in GART and copied to compositor-owned
VRAM with CP_DMA. The compositor receives metadata and completed GPU
storage, never another task's CPU pointer. Imported buffers retain their
Mesa external-BO references across resize and producer closure.

CPU write unlocks refresh retained imports; GPU producer submissions refresh
imports from the producer's VRAM buffer. A registered GPU producer is also
allowed to query its backing metadata. Its first VRAM buffer is initialized
by GPU DMA before partial rendering, and the viewport binding adopts a
backing registered after the initial drawable attachment. This preserves
Preferences toolbars as well as CPU-drawn content.

Backing synchronization holds the accelerator allocation lock through DMA
completion. Unknown completion retains the command buffer, source GART
binding and destination allocation until reboot. Leopard builds require
`com.apple.kpi.bsd` for compositor authorization. Shared browser/GPU process
mode is not required.

Native checks cover cross-process retained updates, resize, producer closure,
alpha, explicit CPU reads, unflushed GL rendering and windowless surfaces.
The strided-copy model covers 3,010 guarded and invalid-input cases, including
a native big-endian run. `tools/guest/backinggputest.c` imports once per size;
its later frames must update without another import. Activity Monitor live
updates and resize, and Preferences pane changes, were checked on .98. The
native NV12 delivery regression passed 4,535,634 exact bytes; Aurora UVD GPU
playback also passed its smoke check. These runtime checks used the fix on
`cf5cab4`, before reconciliation with upstream `a879559`. The reconciled tree
preserves upstream's SurfaceGate default and excludes the new authorization
calls from Tiger builds. It passes host checks and native Leopard builds;
it has not replaced the running .98 driver.

## The Leopard guest (2026-10-11)

`scripts/leopard-guest.sh run|ssh|snapshot|restore` is `scripts/tiger.sh`
with its own disk (`images/leopard.qcow2`), ssh port 2223, VNC `:2`
(5902) and sockets `build/leopard-{mon,qmp}.sock` (`GUEST_QMP` and
`GUEST_SHOT` point `guest-ctl.py` at them). It runs beside the Tiger guest;
no passthrough is offered from it. User `tiger`, the same key.

How it was made, since the obvious ways fail:

- The Sorbet dmg is one HFS+ volume, no partition map: write it into a
  disk with a partition map (`parted mklabel mac`, partition at sector 64)
  and the Open Firmware of QEMU (OpenBIOS) **still cannot boot it**: its
  catalog file is fragmented (8 extents), and OpenBIOS reads only the
  first. Fix: boot the Tiger DVD with the volume as a second disk,
  `diskutil partitionDisk` a new one and `ditto -rsrc` the volume over, then
  `bless --folder .../CoreServices --bootinfo`. The result boots.
- Setup Assistant answers the keyboard step and then stops taking input
  (QEMU's emulated keyboard). Worked around in single user mode (`-s`):
  `touch /var/db/.AppleSetupDone`, a user created with `dscl -f
  /var/db/dslocal/nodes/Default localhost` after
  `launchctl load .../com.apple.DirectoryServicesLocal.plist`, the key in
  `authorized_keys`, `sudoers`, and ssh enabled in `ssh.plist`.
- Xcode 3.1.4: the dmg is plain (concatenate its `.hfs` with the small
  parts, `hdiutil attach -imagekey diskimage-class=CRawDiskImage`), but
  `installer -pkg ... -target /` puts the relocatable parts in `/` instead
  of `/Developer`; move `Documentation Extras Headers Makefiles Platforms
  SDKs Tools` and the Xcode apps into `/Developer` and run
  `xcode-select -switch /Developer`. gcc 4.0.1 (Apple build 5493), gcc 4.2,
  llvm-gcc and the 10.4u and 10.5 SDKs come with it.

### Native kext build (verified)

`LEOPARD_NATIVE=1 scripts/leopard.sh build|check` builds `kext/RadeonNI`
with `make LEOPARD=1` inside the guest. Result: builds first time with
Xcode 3.1.4's gcc 4.0.1 and 10.5's Kernel.framework; the 586 undefined
symbols are **identical** to the Tiger-guest cross-build's; the relocation
check passes; `kextload -t -n` on 10.5.9 says "appears to be loadable" and
`kextlibs` resolves `com.apple.kpi.*` 9.8.0, IOPCIFamily 2.6, IOGraphicsFamily
1.7.3. No card, so nothing was started.

## GL bundle on 10.5 (2026-10-11, no card in the server)

What is known by running things in the guest:

- **The Tiger bundle loads.** The host-built `RadeonNIGLDriver.dylib`
  (Mesa inside, 10.4u SDK) `dlopen`s on 10.5.9: its imports (libz, IOKit,
  libSystem) exist, and 62 of Leopard's 66 `gld*` names are present.
- **Update, same day:** the engine drops a plug-in missing any of the 66
  names, so with 62 the bundle would have been refused. The four are in
  the entry list now (`gld/RadeonNIGLDriver.c`, `gld/rdn_gld.exports`):
  66 of 66 `dlsym`, on 10.5.9 and on 10.4.11. Details in
  `docs/GLD-INTERFACE.md`, "Mac OS X 10.5.9".
- **`gld*` differences.** Apple's X1000 driver on 10.5 exports 66; the
  bundle's table has 63 (62 of them in Leopard's list). New on 10.5:
  `gldCreateProgram`, `gldDestroyProgram`, `gldObjectPurgeable`,
  `gldObjectUnpurgeable`. Tiger-only name: `gldRelatePipelineProgram`.
  Signatures of the four are not known.
- **Dispatch table.** `gliDispatch.h` of the 10.5 SDK has 759 entries
  against 686 in the 10.4u SDK, and the first 686 are the same names in
  the same order: the table only grew at the end. The glue generated from
  the 10.4u header therefore covers the first 686 entries on 10.5 and the
  rest stay the engine's. (Do not generate from the 10.5 header for a
  bundle that must also run on Tiger: it would write past Tiger's table.)
- **Window server hook.** `cglsSetInteger` on 10.5.9 is another function
  (its first words are `mflr r0; cmpwi r3,0; bcl 20,31; ...`), so the
  twelve-word check fails and the hook leaves it alone, as designed. What
  the hook was for (a Core Image backdrop texture) will not work until the
  10.5 function is read.
- **Not known**: the engine context offsets (`rend + 0x4698` table,
  `+ 0x4680` `disp`), the pixel format and renderer info records, and the
  drawable records. Without the card the guest never selects our bundle (the
  kext announces the accelerator only with the card), so these need either
  disassembly of 10.5's `GLEngine` or a run on the G5.

### A Leopard-only bug found: libstdc++ coalescing

The bundle statically links GCC 14's libstdc++. On 10.5 dyld coalesces its
exported weak symbols with the system's `libstdc++.6`, an older ABI:
during `dlopen`, `std::ios_base::Init` (ours) called the system's
`basic_ios::init`, and `locale::_Impl::_M_install_facet` freed pointers it
did not allocate ("Non-aligned pointer being freed" Ãƒâ€”15, from
`_GLOBAL__sub_I_r600_sfn.cpp`). Fix: export only the `gld*` names
(`gld/rdn_gld.exports`, `-Wl,-exported_symbols_list` in
`mesa/target/meson.build`). Relinking the finished bundle with the list
(without rebuilding Mesa) removed the errors on 10.5.9; the Tiger link
accepts the same flag. Not yet seen on a Tiger run: needs a rebuild with
`scripts/build-mesa.sh darwin` from the checkout that has the Mesa tree.

## What was compared (2026-10-11, from Apple's public sources)

Sources: `apple-oss-distributions/xnu` tags `xnu-792.24.17` (10.4.11) and
`xnu-1228.15.4` (10.5.8), `IOGraphics` 179 (10.4.11) against 191.5, 193.2.2
and 233.3, `IOPCIFamily` 79.1 to 162.3. Which tag shipped in which 10.5.x was
not confirmed; every tag up to 233.3 (IOGraphics) and 162.3 (IOPCIFamily) gave
the same answer, and the layouts only move in IOGraphics 305 and
IOPCIFamily 196 (10.6). Reading only; nothing copied.

| Class | Result |
|---|---|
| IOFramebuffer, IOGraphicsDevice | same 97 virtual and reserved slots, same 54 data members (`Point` is `IOGPoint`, typedef'd back to `Point`, same struct) |
| IOService, IORegistryEntry | slots compatible; one reserved slot became a method (below); `IOPMpriv *priv` is `IOServicePM *pwrMgt`, a pointer either way |
| IOUserClient | `externalMethod` took reserved slot 0; ivar block is 32 bytes either way (`void *[8]` against 3 + 1 bytes + `void *[7]`) |
| IOPCIDevice, IOAGPDevice | own slots and data members unchanged through IOPCIFamily 162.3 |
| IOAccelerator | exists in IOGraphics 233.3, no data members |
| IOMemoryDescriptor, IOBufferMemoryDescriptor, IOWorkLoop, IOTimerEventSource | compatible (reserved slots used, none moved) |

Method used (and its trap): a class's vtable slots are its `virtual`
declarations plus one per `OSMetaClassDeclareReservedUnused`; `...Used`
declares nothing and the method that took the slot is declared in its
place. Comparing textual order instead gives false "moves" wherever a
newer header redeclares an inherited virtual (an override takes no slot).

## What breaks, and the stubs

Leopard's 10.4 compatibility libraries (`com.apple.kernel.iokit`, `.libkern`,
`.mach`, `.bsd`, `.6.0`: version 7.9.9, compatible from 1.0.0b1) satisfy the
kext's "6.0" requests. Of the 586 symbols the kext imports, 426 are in the
kernel's export lists (`_IOLockLock` as an alias of `lck_mtx_lock`), 160
come from the PCI and graphics families, and **two do not exist in
Leopard**: `IOService::_RESERVEDIOService4` and
`IOUserClient::_RESERVEDIOUserClient0`. Every vtable of a Tiger-built
subclass points at them. They are `IOService::systemWillShutdown` and
`IOUserClient::externalMethod` in Leopard; `leopard.s` defines the old names
as jumps to those (through CTR, the kernel is out of range of `b`). IOGraphics
does not override `systemWillShutdown`, so the jump is exact.

For the 160 family symbols only names and reserved slots were checked: all
31 `IOFramebuffer`, 17 `IOAGPDevice` and 13 `IOPCIDevice` reserved slots
the kext references are still unused in the Leopard-era headers, and every
method name is in the header. Signatures were not compared.

## Outputs from the VBIOS (2026-10-11)

The first run on a Mac with a retail HD 6670 (`1002:6758`) found no EDID: the
driver only knew the 7570 board's two connectors (DVI-I on UNIPHY0, DDC 0x93;
"DisplayPort" on UNIPHY2, DDC 0x92). The BIOS answered sink type 0x04, dual-link
DVI-D, on the "DisplayPort" lines, and HPD4 read high. Since then
`rdn_output_detect()` (`hw/rdn_modeset.c`) tries, in order: the two built in; the
DisplayPort lines as TMDS (`card->twin`); and the outputs the VBIOS's object
table describes (`board_outputs_read()`, a port of
`radeon_get_atom_connector_info_from_object_table()`: UNIPHY and link from the
encoder object, DDC line from the connector's I2C record, hot-plug pin from its
HPD record through the GPIO pin table). Every path is logged as `board: path ...`.
**First run (same day, a retail HD 6670, DVI direct cable):** the table was
HDMI-A on UNIPHY2 (DDC 0x94, HPD1), dual-link DVI-D on UNIPHY0 link A (DDC 0x92,
HPD4) and VGA on DAC1 (DDC 0x90, no HPD). The EDID came from the DVI port's
line 0x92, found through the "twin" candidate, which pairs that line with the
7570's UNIPHY2: the picture went out of the HDMI port's transmitter and the
DVI port stayed dark (log: `EDID found`, mode set returned 0, no display). Fixed:
with a usable table only the built-in outputs the table also has are tried,
then the table's; the twin is only for a VBIOS with no digital path.
`tests/board_outputs.c` now rebuilds that case (two DDC lines moved in the
7570's connector records) and fails on the old order.

Link B is supported in the transmitter blocks (`rdn_output.linkb`); nothing has
driven a link B output. `tests/board_outputs.c` checks the parse against the
7570's VBIOS: it must return the built-in wiring.

## The kext linker crash (2026-10-11, `8258a77`)

The tester's Mac, a PowerMac11,2 on stock xnu-1228.15.4, took the build that
had loaded at boot the day before and then, after an install and a reboot,
never started it: `kextd: forked load task exited by signal (10)`, and the
same crash in `kextcache`. Both reports: `EXC_BAD_ACCESS (SIGBUS)` at 0x0 in
`kld_file_prepare_for_link + 200` (IOKit.framework 1.5.2), called from
`__kload_load_modules` and `kload_load_dgraph`.

Cause (IOKitUser `kext.subproj/kld_patch.c`, tag 388): `relocateSection()`
caches a relocation record per entry of the sections that can hold vtables
and, for scattered entries and anything but a 32-bit VANILLA, `continue`s
without filling the record; `prepareFileForLink()` then loops over every
entry and reads `rec->fRInfo->r_address`. A **scattered relocation in
`__const` or `__data`** therefore crashes every kext load. Ours came from
`const struct rdn_output *builtin[2] = { &outputs[0], &outputs[1] }`: a
constant array of pointers into another array (pointer plus offset to local
data). The build before it had 168 scattered entries, all in `__text`
(HA16/LO16), none outside. Fixed by assigning the elements in code, and
`scripts/check-kext-relocs.sh` (called by `leopard.sh check` and
`make-g5-package.sh`) refuses any such entry outside `__text`.

The same limit applies to Tiger's `kextload` (the same file in IOKit); rule
for `hw/` and `kext/`: no static data holding the address of an element other
than the first, or of a member, of local data.

## Not done, not known

- ~~A load on a real 10.5~~: done by the user with the hand-patched kext;
  `kextload -t -n` of the native build is "loadable" (above).
- The family kexts' own version requirement: `Info.plist` asks for
  `IOGraphicsFamily` and `IOPCIFamily` 1.0; not checked against 10.5's.
- Runtime behaviour: the window server and `IOFramebuffer` of 10.5 differ
  from 10.4 in ways the headers do not show (mode validation, display
  change notifications, clamshell).
- **Acceleration is not done** (see "GL bundle on 10.5"): the GL bundle follows the Tiger `gld*` interface,
  the window server hook (`cglsSetInteger`, twelve checked words) is
  10.4.11's, and the 2D plug-in is Tiger's. 10.5 would be a new interface
  study (VMsvga2's 10.5/10.6 signatures are the only lead, `[I]` in
  `RESEARCH.md`).
- The Tiger kext does not include `leopard.s`: `systemWillShutdown` does not
  exist there and the link would fail.


### Queued presentation and fence wakeups

The GLD queues double-buffered GPU-backed window presentation when the kernel
advertises fenced surface lifetime support. CPU-backed surfaces, WindowServer,
and older kernels retain synchronous presentation. Surface copies use fenced
work-area slots, and freed VRAM is retired after the last submitted fence.
`RDN_GLD_SURFACE_QUEUE=0` restores the window presentation wait;
`RDN_GLD_SWAP_FINISH=1` forces synchronous swaps.

Completion interrupts remain opt-in with `rdn_irq=1` on Turks/Cayman.
`rdn_irq=0` disables them.
RLC firmware must be present. This starts the interrupt controller, not SMC
power management. Waiters sleep until completion, with a 5 ms safety recheck
and an absolute timeout. IRQ initialization failure retains polling.

Build `tools/guest/fencectl.c` with IOKit and the `hw` include directory.
Mode 0 reports status; administrative modes 1/2 enable/disable interrupts.
Runtime enablement requires RLC preparation during a boot with `rdn_irq=1`.
Turks retains register fences and signals ring interrupts; Cayman uses EOP
fences. The event counter measures ring and EOP completion records. The fallback counter counts completed
waits without a new event observed; it is not a count of GPU hangs.

On the stock 800/600 MHz HD 6670, the initial paired 639x479 clear/swap test
measured 732.893 versus 1978.712 swaps/s with queued presentation disabled versus
enabled (median submission time 1.353 versus 0.347 ms). Both front-buffer pixel
checks passed. This is presentation throughput, not a measured game FPS gain.
Short IRQ tests initially passed, but longer mixed-client runs stalled the GPU;
one also ended with WindowServer crashing in its event thread. Short tests
alone did not establish stability.

The follow-up audit caches `CGSGetSurfaceBounds` and `CGSFlushSurface` entry
points with `pthread_once`, while continuing to fetch current surface bounds.
The cached display-orientation fast path also avoids repeated symbol lookups.
Command-buffer throttling now returns an error after a failed oldest-fence
wait, before allocating or submitting another command buffer. Fault-injection
checks on x86 and emulated PPC cover empty/under-limit queues, successful
retirement, failed waits and allocation suppression. These follow-up changes
are deployed on the HD 6670 test machine. Native G5 fault-injection checks
and 32/64-bit GLD load checks also pass. Game FPS gains remain unestablished.


The first sustained IRQ runs stalled after approximately one to five minutes,
including the register-fence variant. A polling-only control completed 48 mixed
load cycles (about 10.5 minutes), 97,819 window swaps, and all rendering checks,
with no WindowServer restart; peak temperature was 62.5 C at stock clocks.

The next IRQ candidate prepares RLC before CP resume and enables the legacy
INTx ordering read using the existing pinned, DMA-mapped GART dummy page.
Previously RLC started after CP resume and the ordering read was disabled.
This candidate still stalled during cycle 25 (after 24 successful cycles,
about 5.2 minutes). The captured interrupt ring was drained while GPU execution
was stuck. Pending IBs had valid packet boundaries.

Disabling IRQ delivery after boot while retaining RLC completed 48 rendering
cycles, but Jak exited during cycle 30; only the first 29 cycles retain the
full mixed workload. The harness now treats game exit as a failure.

Graphics submission now issues a userspace memory barrier before the kernel
call, matching the decoder path. This orders aperture writes on the submitting
CPU before possible migration. Both PPC GLD builds and native load checks pass;
The first IRQ soak completed 48 cycles (629.8 s), 97,561 window swaps, all
144 offscreen scene checks and 48 presentation checks, with Jak alive throughout
and WindowServer unchanged. Peak temperature was 63.5 C at stock 800/600 MHz.
A second 48-cycle run after IRQ off/on comparisons also passed (630.9 s,
94,525 swaps). Both runs retained the same Jak PID and WindowServer PID, with
192,086 swaps across about 21 minutes of mixed stress. All 288 offscreen scene
checks and 96 presentation checks passed. IRQ event count exceeded one million;
no ring overflow was reported. A final desktop capture showed Jak rendering.
These are bounded HD 6670 tests, not proof of stability on every card or game.

Six alternating old/new presentation trials, with the kernel and IRQ mode held
constant and Jak running, measured median throughput of 272.562 versus 302.659
swaps/s. Median CPU cost fell from 570.054 to 419.146 microseconds per swap.
All pixel checks passed. This measures the combined GLD changes, not an isolated
barrier speedup or a game FPS improvement.

Six IRQ-off/on trials of 64x64 fence-heavy workloads measured median throughput
of 8064/46539 (blend), 6525/10230 (texture), and 301/2399 (ALU) frames/s. Their
small rendering workload emphasizes completion-wait overhead. Three complete
512x512 RGBA scene outputs were byte-identical with IRQs disabled and enabled.
IRQ mode remains opt-in; these hardware results cover the HD 6670 only.


Later failure on the same HD 6670 boot supersedes the bounded soak result:
at 17:35 on 2026-10-24 the graphics queue stalled at fence 1794016 while Jak
remained running. SSH and WindowServer stayed alive; IH read/write pointers
matched, no overflow was reported, and the later temperature reading was 58 C.
Disabling IRQ delivery did not recover GPU progress. Pending IBs and process
samples were captured before recovery. The submission barrier is not a complete
stability fix. The test host was returned to `rdn_irq=0` for recovery; sustained
IRQ mode remains unresolved.

`tools/guest/vramtest.c` holds visible and hidden allocations simultaneously,
writes address-dependent patterns through CP DMA, then reads every word back
through a GART staging buffer. Two complementary patterns detect aliasing and
stuck bits. The test reserves 32 MiB of visible-heap headroom for the desktop
and stops at 85 C. Build with `-Ihw -framework IOKit -framework CoreFoundation`;
`--limit-mib 16` runs a small check before the full test.

On the HD 6670 with `rdn_irq=0`, the full test verified 1,999,728,640 bytes
(1907.09 MiB) in 670 simultaneous allocations, with no mismatches. The highest
allocation ended at `0x80000000`, exercising the top of the 2 GiB address range.
Coverage was 185.91 MiB visible and 1721.18 MiB hidden. The remaining space was
34 MiB reserved by the driver/display, 32 MiB test headroom, a 4 KiB test IB,
and 74.91 MiB unavailable to this client. Reserved and occupied memory was not
overwritten. Temperature stayed at 57.5Ã¢â‚¬â€œ58 C. Subsequent resize/presentation
and three offscreen rendering checks passed.

The initial 16 MiB test after the IRQ game soak stalled at fence 262528 before
verification. Queue buffers and the full aperture were captured. The unchanged
small test passed after rebooting into polling mode. These runs differ in prior
load and boot state, so they do not establish IRQ delivery as the root cause.


### Multiple display heads (2026-10-24)

The `MultiHead` personality option publishes one IOFramebuffer per usable
VBIOS digital output, with separate scanout and cursor allocations. Each head
has its own CRTC, LUT, cursor, hot-plug state and mode list. TMDS heads use
separate PPLLs; DisplayPort heads share DCPLL. An active peer prevents disabling
its shared spread-spectrum clock or taking its lanes for dual-link DVI. Clock
changes update watermarks for the active peer heads under the shared hardware
lock. Passive DP adapters and MST are not implemented.

Surface shape calls retain their framebuffer index. The appended
`RDN_UC_SURFACE_SCREEN` selector returns the selected scanout geometry and VRAM
offset without changing existing ABI structures. The paired GL driver imports
that explicit offset and tracks compositor contexts separately. Secondary heads
use framebuffer software 2D operations because the legacy GA fill/copy interface
still targets head zero; OpenGL scanout stays on the GPU.

The .98 package at `/Users/shay/multimon-cayman-20261024` contains separate
6758/Turks and 6704/Cayman personalities and their respective firmware/VBIOS.
Cayman does not inherit the Turks cold UVD relocation: its UVD session flags
remain disabled pending validation on that card. Startup requests maximum
Cayman fan PWM; SMC power management and IRQ waits remain disabled. The previous
kernel, GL bundle and boot archive are preserved in `rollback`.

Native kernel SHA256: `9fbfed0e8be2a9116740919fab30267917e156bccfb04ce2357815838d4171bd`.
Final fat GL driver SHA256: `3c191c203cf96440fdd9712ba48d3be0d2e24557771467aa4504a2a687ed5b03`.
The boot archive was unpacked and checked against both personality dictionaries
and the kernel binary. Both GL architectures passed native load checks.
`display_banks` passes 21 checks on x86 and qemu-ppc. The native `displaytest`
verifies head 0 at offset zero and head 1 at `0x02524000`, rejecting indices 2â€“6.
The connected DVI head runs at 2560x1600; the disconnected HDMI head has a
640x480 placeholder. Two cold boots, desktop rendering, resize/presentation and
three GPU pixel-correctness scenes passed on the HD 6670. Simultaneous physical
outputs, window spanning and Cayman hot-plug still require hardware validation.


### Multiple graphics cards (2026-10-24)

The subsequent `/Users/shay/multigpu-7570-20261024` deployment adds a 675d
personality. The HD 6670 in slot 1 and HD 7570 in slot 3 are both online at
2560x1600 and 1920x1200 respectively. Only the 6670 has an accelerator service;
the 7570 currently presents through its framebuffer. System Profiler reports
Quartz Extreme unsupported on the 7570. Its Core Image label alone does not
establish that the second GPU executes rendering.

At that deployment, dual-GPU acceleration was unfinished. The kernel's
single-accelerator admission guard was retained because: the target and
OSMesa frontend still use a singleton device/screen, application surface binds
select display zero, and process-wide CGL hooks assume one driver instance.
Loading duplicate GL bundles without coordinating those hooks is insufficient.

The preparatory device API can now open a specific borrowed accelerator service
without falling back to a different card. External VRAM references include their
owning device, and the display refresh throttle is per connection. Native
`devicetest` rejects null and non-accelerator services, then opens two connections
to each published accelerator and checks independent geometry refreshes. It
passes on .98 for the one currently published accelerator (6758); this is not
evidence of accelerated rendering on both cards. These changes are not deployed
as the desktop GL driver.


The subsequent integrated deployment assigns each accelerator a GLD device index
and an isolated Mesa module. Pixel-format display masks select the device;
renderer capability queries remain valid across display masks so WindowServer
can enumerate all renderers. Application surfaces bind through the owning GPU's
callback port. WindowServer reuses engine context addresses across renderers, so
its texture hooks also check the active GL dispatch table. Modern-context opt-in
is process-wide, and make-current/function lookup route to the selected module.

On .98, the V7900 (6704, 2560x1600) and HD 7570 (675d, 1920x1200) both report QE
and hardware CI. Simultaneous legacy and OpenGL 4.6 tests render the expected
triangles on both physical displays and pass pixel readback. Jak II now creates
its Cayman 4.6 context and reaches gameplay. Secondary window shadows still show
black rectangles, and Jak II has visible rendering artifacts; these tests do not
establish complete cross-GPU texture sharing or game correctness.

An unrestricted pixel format exposed another dispatch ownership bug: Leopard
creates per-GPU renderer contexts sharing one engine context. An unattached
renderer could install its early dispatch after the selected renderer attached
its surface. The presentation test then reported GL_DOUBLEBUFFER=0 and returned
black front/back pixels without a GL error. Early dispatch and by-name ownership
now check kCGLCPCurrentRendererID against the module's GPU index. Swap refresh
also reuses its drawable snapshot instead of repeating the same geometry queries.

The fix is deployed in all four fat GLD modules, with rollback binaries in
/Users/shay/presentation-owner-20261024. Newly launched 32-bit tests pass front
readback at 639x479, 1279x719 and 1919x1079, resize stress, and simultaneous
legacy/4.6 rendering on both GPUs. Initial 64-bit ownership tests also pass all
three sizes. Existing processes retain their previously loaded modules.

Presentation timing remains limited by synchronous CGSFlushSurface notification.
A steady-state 64-bit sample attributed 1324 of 1641 main-thread samples to
CGSFlushSurface, including its surface-binding query. Long swap tails remain
under concurrent Jak gameplay; no game FPS gain or eliminated pacing stall is
claimed. The FirePro was thermally limited to about 550 MHz at 86-87 C during
validation. Earlier black-buffer throughput is not a correct-rendering baseline.


### Window migration and Quartz text (2026-10-24)

Quartz applications also use cglsSetInteger(999) to bind a surface as a glyph
shader's backdrop. Install the surface hook in applications as well as
WindowServer, and accept the related private CGLSetParameter imports. Backing
textures use top-down coordinates. Their padded dimensions can exceed the active
drawable; the GPU copy clips the source and clears the padding instead of leaving
an incomplete texture. System Preferences and Activity Monitor now render readable
text, including preference-pane changes that resize the backing.

Unrestricted pixel formats previously created a Mesa context on each GPU without
migrating shared objects. migrationtest uploaded a green texture on renderer
0x1b00, then found glIsTexture false and white output after moving to 0x1a00.
Returning to the original display restored the texture. Ordinary pixel-format
selection now chooses the main display's GPU; an explicit display mask chooses
its lowest selected display, and explicit renderer selection remains available.
WindowServer keeps its per-display renderers. A window retains its rendering GPU
when crossing displays; this is renderer affinity, not cross-GPU object migration.
Presenting on another card can cost an additional transfer.

All four fat GLD modules were rebuilt and installed in
/Users/shay/dual-regression-20261024, which retains the pre-change rollback copies.
The default migrationtest passes all four crossings with exact RGB 32,192,64 and
no GL errors. Separate per-display contexts still select 6704 and 675d and pass
20 seconds of simultaneous triangle rendering and pixel readback (1503 frames).
System Preferences was moved between displays and repainted through the Sound
and Appearance panes; text remained readable on the secondary display. That
initial validation left black secondary window shadows unresolved and did not
restart WindowServer. The real-drag regression below required additional fixes.


The endpoint test above was insufficient: a real mouse drag of a freshly
restarted Aurora still produced a white window on the second GPU (2026-10-25).
A readback of the native video texture was correct. Subsequent compositor draws
used empty window and shadow textures (0 by 0), overwriting it with white tiles
and solid black shadows.

WindowServer's CGL virtual screens now share a texture namespace and revision
registry across the GLD modules. Each GPU retains its own Mesa objects. Rectangle
textures are copied on first use or after a change, preserving alpha and sampler
settings. Deletion releases resident copies, and the last context releases the
registry entries. This bridge covers WindowServer's level-zero rectangle
textures; ordinary application contexts still retain renderer affinity.

The native-surface fallback also read the source card's uncached VRAM through
memcpy. WindowServer spent 269 of 288 main-thread samples in that upload, while
Aurora waited in CGSFlushSurface. A guarded upload path now recognizes the other
card's active surface read lock by its dimensions and stride. Mesa copies the
source image into a GART staging resource before the CPU reads it, then uploads
it to the destination GPU. This remains a transfer through system memory, not
PCIe peer-to-peer access. Both cards render and composite with their GPUs.

On .98 with FirePro V7900 primary and HD 7570 secondary, a warmed 640x360/30 fps
YouTube stream in a 1217x861 Aurora window previously dropped 32 of 33 frames
in five seconds on the secondary display. With the GPU-assisted transfer, the frame counter advanced by
150 with 2 dropped over five seconds; primary and return-to-primary
measurements each advanced by 150 without new drops. A real mouse drag
from (427,160) to (-1773,160), followed by six seconds of playback, advanced
126.329 to 138.399 seconds with 362 frames and 3 new drops. The window, shadows,
controls and video remained visible. These are window-transfer measurements,
not a 1080p60 decoding claim.

Activity Monitor and System Preferences were dragged across both GPUs and back.
The secondary Activity Monitor continued updating its process rows and graphs
with readable text; System Preferences labels and shadows remained intact.
The surface user client also retains a foreign producer accelerator for read
locks when opened through the destination GPU; probes read identical pixels
through both services. WindowServer and applications must restart to load the
new GLD modules. Rollback copies are under
/Users/shay/drag-regression-20261025 (RadeonNIGLDriver*-before, rollback.kext and
rollback.mkext). The browser framework was not changed for these fixes.

The final cleaned build was reloaded in WindowServer and retested with actual
mouse drags in both directions. Forward playback advanced 8.095 seconds with
243 total frames and 4 drops; reverse advanced 8.010 seconds with 240 frames
and 11 drops. Resizing to 1500x1000 across the display boundary kept both
halves correct, but recorded 46 drops among 177 total frames over 5.695 seconds.
Cross-GPU transfer performance remains a limitation for larger spanning windows.

### Cross-GPU transfer and completion follow-up (2026-10-25)

The foreign surface upload now holds a mapped GART staging resource until the
receiving GPU's synchronous texture upload consumes it. RDNGLMapSurfaceV2 and
RDNGLUnmapSurfaceV2 remove the intermediate full-frame malloc/memcpy. The source
resource and transfer remain retained through the upload; every successful map
is unmapped on both success and failure. This still uses CPU/system-memory
transfer, not PCIe peer-to-peer DMA.

Quiet native presentbench runs, 1217x861, swap interval zero, V7900 renderer
0x1b00 producing for the HD 7570 display:

| Position | Previous copy path (swaps/s) | Mapped path (swaps/s) |
| --- | ---: | ---: |
| Primary | 742.993 | 700.266 |
| Secondary | 45.934 | 110.783 |
| Spanning both | 48.776 | 104.093 |

All runs passed the front-buffer pixel check. These are submission-throughput
measurements, not displayed refresh rates or game FPS. The initial baseline with
Aurora running is excluded from this comparison. Actual mouse drags in both
directions and a 1500x1000 spanning browser window remained visually intact.
The spanning video interval advanced 157 total frames with one drop over 5.237
seconds. Later playback reported a 1920x1080 source; adaptive YouTube quality
was not locked, so this is not a controlled decoder comparison.

Memory allocation now reclaims completed cached allocations before a blocking
trim. A busy cache entry no longer prevents reuse/reclamation of later completed
entries. GART chunks reserve alignment slack beyond the kernel's page alignment
only, fixing the extra page that broke exact-budget allocations. Host and
big-endian PowerPC tests cover allocation pressure, failed-completion retention,
exact-budget allocation and alignment/overflow cases. Native bufferpressure
verified 360, 372, 384 and 396 MiB of live PBO storage, GPU texture uploads from
the final MiB, and reuse. Final runs explicitly verified renderer 0x1b00/mask 1
and 0x1a00/mask 0x10. Earlier renderer-ID-only runs selected the primary twice
and are excluded from secondary memory validation. At that point live VRAM eviction was not implemented. The stable-alias
implementation below supersedes that limitation.

The IRQ action now advances IH_RB_RPTR while consuming records and checks for
arrivals during the drain. Simulated arrival, wraparound and overflow cases pass
on x86 and PowerPC. Kernel ccb3e3d12390a1ff9dff6ac745a8f97a136bace1f316e7f2e644be19343223cc
booted with IRQs active on both cards. Nine mixed-load iterations passed 18,000
explicit fence waits plus presentation/resize and memory checks without hangs
or pixel failures. Cayman recorded 452 fallback completions early, then held
that count. A later three-iteration run with Aurora video playing raised it to
517, reaching 893 during continued playback before IRQ shutdown; the HD 7570
remained at zero. Thus fallback completion is still needed;
this does not establish reliable interrupt-only completion. Runtime IRQs were
turned off afterward and boot arguments restored to -v rdn_dpm=0 rdn_irq=0.

Artifacts and rollback copies are in transfer-irq-memory-20261025 under the
host h264-validation-20261022 directory and /Users/shay on .98. The kernel
installer preserved existing personality/firmware data and verified the boot
cache binary. Network interruptions after reboot were accompanied by Ethernet
link-bounce messages while uptime continued; they were not GPU hang evidence.


### Live eviction and backing-store regression fixes (2026-10-25)

Private hidden allocations now use stable GART aliases for live eviction and
promotion; see MEMORY-EVICTION.md for scope and limits. Foreign surface textures
can sample mapped source staging pages on the destination GPU, retaining their
lease until GPU completion and successful unbinding. Source GPU-to-RAM transfer
remains necessary. WindowServer texture-version synchronization still has a
CPU readback/upload path.

The corruption investigation reproduced visible-heap exhaustion on Cayman:
64 KiB ordinary allocations failed while hidden allocations succeeded. Command
buffers now have an isolated 8 MiB pool, and the visible cache is limited to
8 MiB. commandpressure exhausted ordinary visible allocation on both 6704 and
675d, then successfully allocated, submitted and retired a reserved command.
A mirror-refresh error no longer invalidates an already accepted submit fence.

New private color buffers and newly allocated backing stores are GPU-cleared
before publication. Existing single-buffer drawable attachments are retained.
The backing import's temporary context now carries its Mesa context; without
it, the new clear rejected imports with 0x2717 and left applications white.
backingsurfacetest passes on both GPUs, and System Preferences and Activity
Monitor rendered correctly after WindowServer reloaded the fixed bundles.

Final 1217x861 swap-interval-zero runs passed initial-black and final-color
checks at 590.485 swaps/s on the primary, 122.316 on the secondary, and 121.615
spanning both. Repeated spanning-window resizing passed for eight seconds.
These are submission rates, not displayed FPS or a controlled game comparison.
Legacy MPHWMoveRegion/MPHWCopyRegion unsupported errors remain in WindowServer.
Evidence is in h264-validation-20261022/command-reserve-20261025 and
live-eviction-transfer-20261025 on the build host.


### Renderer identity and migration validation (2026-10-25)

GL_RENDERER now reads the model property from the selected accelerator's
registry ancestry, falling back to Mesa's renderer description if unavailable.
The existing RDN_RENDERER override is retained. The Leopard dispatch reports
its GL 2.1 / GLSL 1.20 API ceiling while retaining the backend's actual Mesa
build identification. The explicit modern-context entry points retain their
backend version strings.

Native multigputest identified 6704 / renderer 0x1b00 as ATI FirePro V7900 and
675d / renderer 0x1a00 as ATI Radeon HD 7570. Both reported Mesa 26.2.4, passed
front-buffer color checks, and ran together for twenty seconds.

Automatic application renderer migration is not enabled by default.
RDN_GPU_SWITCH_TEST bypasses the single-renderer preference for diagnostic
processes only. A native all-display virtual-screen test exposed two renderers;
switching from 0x1b00 to 0x1a00 lost a previously created 2D texture and rendered
white. Returning to 0x1b00 restored the correct pixels. The existing texture
registry synchronizes only WindowServer rectangle textures, not arbitrary
application texture levels, buffers, programs, display lists or context state.
Those must survive migration before the diagnostic path can become the default.
Cross-GPU frame presentation remains enabled independently of renderer migration.


The diagnostic migration path now shares application texture revisions and
imports them when the destination drawable becomes current, before object
queries. It supports level-zero RGB/RGBA 2D textures; mipmapped and other-format
application textures are rejected by this snapshot path. Generated texture
names, later changes, and deletion use the shared registry. The registry ABI
is unchanged from the existing WindowServer path.

On .98, migrationtest --switch now alternates 0x1b00 and 0x1a00 through actual
window moves, preserves glIsTexture, and produces the expected pixels. The
--state mode additionally requires existing VBOs, display lists, bindings and
fixed-function state to survive; it still fails on the destination renderer.
RDN_GPU_SWITCH_TEST remains diagnostic-only. General application migration is
unfinished; do not enable this path globally or describe the texture test as
proof that games can migrate safely.


### OpenMark legacy-context switching follow-up (2026-10-25)

The diagnostic path now replays recorded display lists into each renderer's
local Mesa namespace. Bitmap commands copy their input with the original unpack
alignment, row length, skips and bit order; replay restores the destination's
unpack state afterward. This fixes the missing font/FPS overlay seen on the
secondary display in OpenMark. Fixed-function attributes, matrix stacks, texture
bindings and mutable buffer contents are copied at renderer transitions.

OpenMark queries GL_RENDERER during initialization and retains its pointer
(verified in the PowerPC executable). Migrating contexts now return a stable
per-context string buffer whose contents follow the active device's registry
model. Explicit RDN_RENDERER overrides remain unchanged.

Applications listed by executable name in
/Library/Application Support/RadeonNI/gpu-switch opt into this path on normal
launch. OpenMark is the deployment target; other applications retain the
single-renderer preference. RDN_GPU_SWITCH=0 disables migration for a process;
RDN_GPU_SWITCH_TEST remains an explicit diagnostic override. This is not yet
general shader/VAO or arbitrary-resource migration: the legacy-state importer
rejects unsupported state, and the texture snapshot covers level-zero RGB/RGBA
2D storage. Do not add general games to the application list based only on the
OpenMark result.

The migration test now checks renderer changes in both directions, cached name
pointers, texture updates and deletion, VBO contents, retained fixed-function
state, display lists, and bitmap glyphs with non-default unpack state. OpenMark
was also moved between the physical displays and its output captured. Evidence
is under command-reserve-20261025 in the existing validation directory.

Final native checks passed both migration modes, including legacy numeric RGB
internal format 3, buffer contents, cross-renderer list deletion, and bitmap
unpack with row padding, skipped rows/pixels and LSB-first data. OpenMark was
relaunched without diagnostic environment variables and switched repeatedly in
both directions. Final captures show its texture, text/FPS overlay and changing
FirePro V7900 / Radeon HD 7570 label on the corresponding displays. The app
remains running; only OpenMark is enrolled in the normal-launch policy.


## Quartz menu regression qualification (2026-10-25)

The corrected FirePro renderer name exposed Leopard Core Image's name-based
hardware classification: `ATI FirePro V7900` falls outside its Radeon prefix.
WindowServer now receives `ATI Radeon (FirePro V7900)`, derived from the detected
model; applications retain `ATI FirePro V7900`. Renderer IDs and virtual-screen
selection are unchanged. `tests/test_renderer_classifier.py` covers the public
name, WindowServer compatibility label, Radeon models and explicit overrides.

Quartz bitmap filters also read same-task backing memory after GL flush without
an IOAccel read lock. The GPU-backing path had left that memory stale. Backing
attachments now retain the CPU address for WindowServer and publish rendered
pixels before returning. Aligned top-down backings are imported as pinned GART
render targets and updated by GPU copy plus fence completion. Other layouts use
the existing readback path. Ordinary application backings retain explicit-lock
ownership. No kernel or clock/power setting changed.

The extended native `backingsurfacetest ... quartz` reproduces the stale bytes on
both Cayman and Turks before the fix and passes afterward, including aligned
backings, alpha, CPU edits, resize, unsubmitted producers and windowless sharing.
The ordinary backing tests also pass. Universal PPC32/PPC64 GLDs were rebuilt and
installed on .98; native backing validation used PPC32. After WindowServer reload,
full-resolution captures confirm restored menu-bar translucency, complete Finder
File menus, right-click menus on both displays, and clean dismissal on the primary.
The older small dark patch behind the Apple menu icon remains; this does not
claim complete Quartz/Core Image conformance.

Evidence: `h264-validation-20261022/command-reserve-20261025/qe-*` on the build
host. Final GLD1 SHA256:
`3481d82a54ffde6478e95c69496476e5080beb6c9af756b1aba19783c2c6ada8`.

Fullscreen attachment also now recognizes explicit surface ID zero as the
selected GPU's validated scanout. Unknown nonzero surface IDs still fail.
`test_fullscreen_target.py` and native AGL fullscreen creation pass at 2560x1600.
The first full OpenMark run was interrupted to prioritize the Quartz regression;
it is not a completed benchmark result. A second attempt after the UI fix also
reported `test stopped` before completing; no full score is claimed.

### Driver Manager: per-card telemetry

The manager uses each PCI device ID and its own framebuffer telemetry and accelerator connection. Turks/7570 requested voltage comes from `RequestedCoreVoltageMV` when the software idle governor is inactive; governor-managed states use `PowerCoreVoltageMV`. Live clocks remain hardware readback. The non-Cayman UI reports fixed BIOS clocks or software governor state instead of suggesting Cayman SMC controls are available. Missing tachometers, measured rail backends and power sensors remain explicitly unavailable; requested voltage is not an ADC measurement. Fence completion tests bit 0 of the status mask (bit 1 denotes capability).

For a native GUI-session diagnostic snapshot, launch `RadeonNIManager --report /absolute/output.txt`; it records both cards after three seconds and exits. This uses the same report as the Copy diagnostic report button.

### Matched universal GLD packaging

Build all four renderer modules for both PPC32 and PPC64 from the same overlay. Both Leopard build scripts now request every module explicitly. `build-mesa-leopard64.sh` accepts `RDN_PPC32_DIR` and uses `scripts/package-leopard-gld.py` to package all four modules together. `RDN_PPC32_SLICE` remains an alias for its containing directory. A PPC64-only build prints that it is not an installable universal package.

The packaging command is `python3 scripts/package-leopard-gld.py OUTPUT --ppc32 PPC32_DIRECTORY --ppc64 PPC64_DIRECTORY`. It validates thin input architectures, Mach-O bounds, and required context/async-video exports before writing any module. The output includes per-module and per-slice SHA256 hashes in `manifest.json`. Running the same command with only `OUTPUT` verifies the complete package against its manifest. Thin or incomplete-ABI inputs are rejected. On native Leopard, `lipo` requires the file before `-verify_arch`; G5-targeted PPC32 slices have subtype `ppc970`, rather than generic `ppc`.

### Canvas map failure investigation

Fresh native 24-byte PBO read maps, GPU copies, pixel readback and native-order indexed/non-indexed indirect draws pass. A speculative CP-DMA byte-swap candidate failed its native test and was rejected; native-order draw arguments are correct on these cards. The rejected patch is retained only in the diagnostic artifacts, not in the source patch series. Current diagnostics retain the IOKit submit error and report the failed BO's placement, access flags and fence state. Browser isolation traced later failures to Dawn indirect-draw processing. PPC validation batches must serialize the 64-bit index-buffer size, draw count, flags and argument offsets in WebGPU little-endian order. Native uploads misread the draw count and bypassed indexed validation. The browser now serializes these batches, adds the GLSL base instance to the shader instance index, and issues equivalent GPU draws from arguments already decoded on the CPU. Indexed draws include the bound index-buffer offset. Native indirect commands remain bypassed in this browser path; this does not claim a general driver fix for them.

On .98, the expanded browser regression passes compute, uniform/storage buffers, atomics, indirect dispatch, rendering/readback, queries, GPU-generated indirect draws, nonzero argument/index offsets, negative base vertex, nonzero first instance, invalid-index suppression and zero-count draws, followed by a 12-second canvas animation. A second run without the optional first-instance feature correctly suppresses those draws. Both GPU queues pass 1,000 fence submissions afterward; shader, map and submit failure logs remain empty. The kernel binary is unchanged. Evidence and the incremental browser patch are in `h264-validation-20261022/command-reserve-20261025/`; the reusable fixture is Chromium's `content/test/data/gpu/webgpu_indirect_ppc.html`.


### Reset notification correction (2026-10-25)

The expanded Aurora indirect regression passed, but loading further WebGPU demos
still stalled the V7900. The 7570 continued completing fences. This is not a
qualified fix for the underlying GPU hang.

The winsys reset callback was an unconditional `PIPE_NO_RESET`, and the modern
GL bridge created contexts without loss notifications. The device now latches
confirmed `NotReady` submission failures and queries the accelerator fault flag;
the winsys reports an unknown-context reset without claiming recovery or guilt.
Modern contexts request loss notifications. Legacy contexts retain their existing
reset strategy. Faulted waits do not report successful completion or retire memory.

Matched PPC32/PPC64 GL modules are installed. A native GL test on the existing
faulted boot reports `GL_UNKNOWN_CONTEXT_RESET` (0x8255) on the V7900 and no reset
on the healthy 7570; both modern contexts report `GL_LOSE_CONTEXT_ON_RESET`
(0x8252). Host reset, IRQ, client-lifetime, and 304 winsys synchronization checks
pass. The kernel additions publish the fault through fence-control flag bit 2
and cover previously missing IORegistry publication paths; these kernel additions
are not deployed yet. The installed kernel is unchanged. Evidence is in
`command-reserve-20261025/reset-{healthy,fault}-gl.log` and
`reset-notification-universal/manifest.json` in the local validation directory.

### Experimental half precision and vertex storage (2026-10-25)

`R600_FP16_EMULATION=1` enables explicit half precision using GPU 32-bit ALU
operations with half rounding. Packed SSBO, shared and scratch accesses are
lowered to aligned 32-bit operations; partial stores preserve adjacent halves.
Odd-sized LDS allocations round up, and workgroup barriers remain after split
LDS stores. Half literals and constants use the correct union members on PPC.
Automatic mediump lowering remains disabled.

`R600_VERTEX_SSBO_READONLY=1` exposes eight read-only vertex SSBOs and eight
read-only vertex images using the VS/LS resource bank. Writable vertex resources
remain unsupported. Both options remain opt-in.

Matched universal GLDs are installed on .98. Thirteen native probes pass on
both PPC32 and PPC64, on Turks/7570 and Cayman/V7900: 52 runs, 2,816 checked
results. Coverage includes conversion/rounding, packed vectors and matrices,
Dawn-order uniform uploads, odd buffer/LDS sizes, cross-wavefront barriers,
SSBO range/length/replacement, and float/uint/3D vertex images. This is targeted
qualification, not full conformance. Half vertex I/O, scratch spills and
multisample vertex images still require qualification. Evidence is in
`command-reserve-20261025/shader-*-suite.json` and the native logs.

The matched build completed all 21 baseline UnivBench samples on the V7900.
A second run with both options enabled recorded 21 valid samples, but returned
exit code 1; it is not an unqualified benchmark pass. Both cards then completed
1,000 fences. The earlier V7900 hang and null GL dispatch crash remain unresolved;
these runs do not establish a permanent hang fix. The kernel binary is unchanged.
The fence-timeout logger source now reads the VRAM completion slot when memory
fences are active instead of printing the old scratch register. That diagnostic
change is not deployed.

Aurora's read-only storage-texture port now includes reflection, GL bindings,
resource usage/alias validation, GLSL image loads and the size/layer builtins
used by robust bounds checks. The shared-image factory follows Dawn's default
Leopard GL toggle instead of requiring a redundant explicit enable flag.

A 600-frame 1920x1080 canvas resize test reproduced GPU-process loss from VRAM
exhaustion. Dawn's GL texture destructor checked `OwnedInternal` after the base
destructor had changed the state to `Destroyed`, so it never deleted the GL
allocation. Ownership is now captured before the state transition. The same
FirePro test passes with sampled device-wide allocations of roughly 194-227 MiB
while running, correct pixel readback, and healthy fence completion afterward.
The 7570 also passes all 600 resize frames with roughly 110-151 MiB allocated.
The browser fixture is `content/test/data/gpu/webgpu_vertex_storage_ppc.html`;
`?frames=600&width=1920&height=1080&resize=1` enables the regression workload.
This fixes the reproduced browser resource leak, not the unresolved native
UnivBench GPU hang described above.

The verified browser framework is installed in both `/Applications/Chromium
Aurora.app` and the volume-root Aurora app on .98; unchanged matching GL
libraries were hash-checked. The QA app has the same build. Deployment hashes
are recorded in `browser-read-images-installed.json` and
`browser-production-installed.txt` in the validation directory. No kernel
replacement or reboot was needed for the browser lifetime fix.

## Family compatibility audit (2026-10-26)

The driver has accelerated Turks/Cayman paths and experimental Barts/Caicos
framebuffer paths, not support for every TeraScale 2/3 ASIC. Linux v6.12
`include/drm/drm_pciids.h` supplies `hw/rdn_device_ids.h`: initially 41 Turks/
Cayman identities, now 69 across four families (51 non-mobile). These are
silicon classifications, not 69 hardware validation results.

Previously `rdn_card_init()` and the Mesa winsys recognized Cayman only as
`0x6704` (V7900). Other Cayman IDs fell through to Turks, including Mesa's
shader family and graphics-IP version. Both layers now use the shared table.
An actual unknown PCI identity is rejected before AtomBIOS initialization;
the winsys also rejects unknown device IDs instead of reporting Turks.
Offline ROM tests without PCI callbacks retain their existing behavior.
`ASICFamily` records the selected family in the IORegistry.

`scripts/configure-device-families.py` prepares separate IOKit personalities
from that table without installing them. Example, on the build host:

```
python3 scripts/configure-device-families.py --families TURKS CAYMAN \
  --firmware-dir /path/to/firmware --input /path/to/Info.plist \
  --output /path/to/review/Info.plist
```

It requires each selected family's PFP/ME blobs with the sizes required by
the command processor. MC/RLC/SMC blobs are taken only from that family's
filenames when available. UVD is not enabled by inventing a compatible blob.
Omit `--firmware-dir` for framebuffer-only personalities. Profiles exclude
mobile IDs, inherit only common interface properties, and do not copy a
board VBIOS or experimental memory-controller/power settings. The kext
checks `FirmwareFamily` against PCI classification before accepting a ROM.
The forced fan-PWM startup quirk stays restricted to the tested V7900.

The original accelerated personality is unchanged. The stock Info.plist now
adds Barts/Caicos framebuffer-only personalities for 20 further desktop IDs.
Generated full-family profiles are build/review inputs, not installed or
hardware-certified here.
Do not pass them through the legacy `g5/install.sh` firmware expansion: that
installer still inserts Turks blobs into the first personality. Integrating
family profiles into the shipping installer remains release work.

Other limits found in the audit:

- Cypress, Juniper, Redwood, Cedar and integrated APUs remain unimplemented.
  Barts/Caicos have explicit framebuffer descriptors and are not permitted
  to enter the Turks acceleration path.
- `hw/rdn_mc_layout.c` still restricts experimental rebasing to three IDs,
  two memory sizes and known initial address windows. Keep these transaction
  guards until new layouts are modeled and verified; deleting the whitelist
  would not provide reliable VRAM compatibility.
- `rdn_display_init()` now initializes BIOS-described transmitters on both
  families; see the transmitter-initialization follow-up below. The fixed
  wiring fallback is restricted to the tested Dell 7570 subsystem.
- The Linux device harness now reads PCI configuration before mapping GPU
  resources, selects family-specific firmware, and rejects families with
  no implemented accelerated path. It is still an acceleration harness,
  not a Linux framebuffer test for the new families.
- Open Firmware discovery now recognizes non-mobile Turks variants from
  the table. Cayman preboot output and other Boot ROM versions are not
  enabled by this change. RomProbe's separate target selection is unchanged.
- Fused-off resources are partly read by ASIC initialization, but the winsys
  still reports a full render-backend enable mask and a placeholder CU count.
  More accurate topology needs an explicit device-ABI change, not guessed counts.

Validation: exhaustive native and big-endian PowerPC/QEMU classification;
real card-init source exercised with a stub ROM parser (unknown vendor/family
rejected before parsing); separate family-profile and microcode-size tests;
host hardware-library build; existing 49-case power and 21-case display-bank
models; and PPC64 Mesa winsys object compilation. No driver was installed,
no GPU was exercised, and no additional board is claimed tested. A full kext
build and final package/runtime validation remain required.

### Board-driven transmitter initialization (2026-10-26)

Transmitter initialization prepares the physical UNIPHY output blocks used
by DVI, HDMI and DisplayPort. The BIOS INIT command needs the connector type,
transmitter selector and link A/B wiring for this particular board.

`rdn_display_init()` now loads the AtomBIOS object table when detection has
not already populated it, and uses that board's outputs on both Turks and
Cayman. DisplayPort outputs run before TMDS outputs, retaining the previous
7570 ordering without injecting its ports into other boards. Repeated entries
with identical INIT parameters run once; separate links and connector types
remain distinct. All entries are validated before the first command, and a
firmware-command failure stops subsequent initialization and clock setup.
`rdn_output_init()` also rejects invalid transmitter descriptions.

Without a usable digital-output map, fixed wiring is allowed only for the
recorded Dell HD 7570: PCI 1002:675d, subsystem 1028:2b20. Other boards return
an error rather than guessing the 7570 wiring; Cayman no longer silently
continues with no transmitter initialized. The known-board fallback retains
its additional explicitly selected output, including the legacy adapter path.
The display-engine clock programming and command-table revision checks are
unchanged.

Validation: `tests/transmitter_init.c` exercises the real public display-init
path with synthetic AtomBIOS object tables and wrapped command execution.
Cases cover both families, rewired DVI/HDMI/DP ports, duplicate entries,
independent A/B links, invalid maps before commands, command failure, lazy
object-table loading and subsystem-qualified fallback/rejection. Existing
21 display-bank, 49 power-model and DVI link-count checks pass. Modeset
objects compile for PPC32 and PPC64 Leopard. Hardware output, full kext
link/load and firmware replay with the private ROM remain untested here;
no live driver was changed.

### Experimental Barts/Caicos framebuffers (2026-10-26)

The shared three-card bring-up is sufficient to extend implementation by ASIC
family. Barts and Caicos use the same DCE5 display generation as Turks; Linux
v6.12 groups them in `btc_asic` and provides the controller limits used here.
The claim is **implemented experimental OS X framebuffer support, hardware
unverified**, not a successful boot on an absent card or full acceleration.

| Family | Desktop matches | Display controllers | Implemented policy |
| --- | ---: | ---: | --- |
| Turks | 16 in full-family generator | 6 | Existing accelerated path |
| Cayman | 15 in full-family generator | 6 | Existing accelerated path |
| Barts (including HD 6850/6870 IDs) | 9 in stock Info.plist | 6 | Experimental framebuffer |
| Caicos (including HD 6450 IDs) | 11 in stock Info.plist | 4 | Experimental framebuffer |

Barts/Caicos profiles use the board's own AtomBIOS for cold POST and memory
initialization, BIOS connector wiring and the common DCE5 modeset/CPU scanout
path. They ship without GPU firmware or an accelerator request; default
hardware cursor and multiple-head publication are disabled. `FirmwareFamily`,
`FramebufferOnly` and `DisplayControllerCount` expose the selected policy.
They do not inherit a tested board's VBIOS, MC relocation or power settings.

Physical CRTC limits now apply to POST/hang detection, modesetting, LUT and
cursor updates, bandwidth updates, MC loops and kernel head publication.
Caicos never accesses CRTC banks 4/5 through these paths. DIG encoder capacity
is a separate limit and stays at six; it is not conflated with CRTC count.

Kernel accelerator publication, CP initialization, direct GPU initialization,
Mesa winsys creation, manual clock changes, MC firmware upload and VRAM
relocation all reject the framebuffer-only families. Memory setup comes from
the board's ASIC_Init table, not Turks MC tables. Existing reboot-required
provider flags remain honored. A bad or unsupported BIOS command revision
still fails explicitly, and the fixed-wiring fallback remains Dell-7570-only.
The Open Firmware client still discovers Turks only; no new power-on console
support is claimed, and unsupported families no longer receive its install
notice. Digital DVI/HDMI/DP outputs are in scope; analog VGA is not added.

A pre-existing C++ build blocker was also resolved: the embedded SMC state
has a zero-length tail in C++ and retains its flexible tail in C. The C/C++
firmware layout is identical (SWSTATE size/offset 4/4; state table size 3028,
`driverState` offset 656, `dpmLevels` offset 660). No SMC wire bytes changed.

Validation covers exhaustive x86 and big-endian PowerPC/QEMU identity checks,
family-specific profile generation and stock plist matches, cold/warm POST
routing with modeled ASIC_Init success/failure, controller-bank access bounds,
zero-I/O rejection of acceleration/MC/clock requests, and four-family synthetic
AtomBIOS transmitter initialization. Existing display, power, Cayman DPM/SMC,
MC layout, GART and UVD models pass. Eight changed hardware modules compile
for PPC32 and PPC64, the PPC32 Leopard kernel C++ object compiles, and the
PPC64 Mesa winsys/Linux device harness objects compile. The complete kext
bundle has not been linked, loaded or tested on Barts/Caicos hardware.

No new board has been rated hardware-tested, no driver was installed and no
Open Firmware/NVRAM change was made. These changes remain unstaged for review.

References: Linux v6.12 `drivers/gpu/drm/radeon/radeon_asic.c`,
`drivers/gpu/drm/radeon/radeon.h`, and `include/drm/drm_pciids.h`.

### Cayman occlusion query ordering

Mesa patch `0112-r600-cayman-query-sample-order.patch` emits
`PS_PARTIAL_FLUSH` before the start and end `ZPASS_DONE` samples on Cayman.
Reserve two additional command words for each sample. Flushing caches after
query end did not prevent the reproduced V7900 stall.

`tools/guest/querytransition.c` is a PPC32 Carbon/AGL regression probe. Link
with Carbon, AGL and OpenGL. It requires the V7900 renderer, overlaps four
query objects, and checks sample counts and framebuffer pixels. Run
`querytransition MODE ROUNDS` with `RDN_QUERY_TRANSITION_FLUSH=0`.
Modes: 0 disables queries; 1 uses discard/MRT1; 2 disables discard; 3 writes
MRT0; 4 masks the shader output; 5 disables all color writes; 6 disables
stencil; 7 disables depth and stencil. Use separate processes and thousands
of rounds: short runs passed with earlier defective drivers.

The native PPC32 matrix passed 36,864 queries across modes 1�6, plus 4,096
no-query rounds. Another captured run passed 4,096 queries and confirmed
flush-before-sample packets. Both PPC32 and PPC64 slices build; this does
not constitute native PPC64 or general application stability validation.

A later GTA city run still stalled after approximately 41 minutes with
patch 0112 installed. All 16 query samples in the pending IB had the flush
present. Treat this patch as passing the focused regression, not as a
complete GTA hang repair. The 283-draw IB does not identify the stalled draw.
