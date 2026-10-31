# Framebuffer power management on Tiger: why sleep hangs, and what to build

Written 2026-10-08 (research, nothing built yet). Read with `docs/HOTPLUG.md`
and the journal entries "display sleep, first try" and "System sleep".

## System-sleep implementation audit (2026-10-25)

Source inspection of the V7900 worktree; no sleep test or hardware change.
The opening research below is historical: power-state registration now exists.

### Current boundary

`RadeonNI::enableController()` registers three states, all carrying
`kIOPMPreventSystemSleep`, and publishes `SystemSleepSupported=false`.
`setAttribute(kIOPowerAttribute)` delivers display power notifications and
calls `setOutputPower()`. This disables/reprograms the output; it does not
suspend the accelerator, preserve VRAM, or restore a powered-down PCI device.
`kIOSystemPowerAttribute` has no driver-specific shutdown/restart handling.

### Required lifecycle

Use one coordinator per physical card, shared by its framebuffer heads:
`running -> quiescing -> suspended -> restoring -> running`, with a separate
failed state. Display DPMS remains independent of system sleep.

1. Gate every submission and GPU/MMIO maintenance path. Stop hotplug polling,
   power/governor timers and video admission; drain graphics and video work
   with bounded waits before disabling fence interrupts. Do not wait while
   holding a lock needed by the completion path.
2. Preserve live allocation identities, surface metadata and fence progress.
   Save live VRAM contents, scanout, cursor and translation data in system RAM
   unless retention across the G5's actual sleep transition is demonstrated.
   Hidden VRAM needs a staged GPU-to-GART transfer before the engine stops;
   the CPU aperture cannot cover the whole V7900. Budget snapshot storage
   before accepting sleep and retain pinned system-memory mappings.
3. Stop video, DPM/SMC, engines and translation in dependency order. Coordinate
   PCI power and framebuffer mapping redirection with IOGraphics. Verify the
   target Tiger/Leopard notification and acknowledgement ordering before
   wiring the callbacks; long operations need a suitable deferred context.
4. Restore PCI access, POST/memory-controller state, firmware, translation,
   engines and saved allocations before restoring modes, links, gamma and
   cursor. Reinstate interrupts and power policy, then reopen submissions and
   complete wake notifications. A failed restore must not report a usable GPU.

### Reuse and traps

- `rdn_gart_resume()` is reusable after restoring its page table; it does not
  preserve the table or client allocations itself.
- `startEngine()` resets surface-mirror state and initializes heaps;
  `rdn_accel_init()` clears the accelerator structure, including fence state.
  Split cold initialization from resume; keep userspace handles and logical
  fence progress valid across the transition.
- `retire()` terminates services and destroys allocation state. It is not a
  suspend primitive. Keep `fRetiring` separate from reversible sleep gating.
- `stopHardwarePower()` releases its state even after an SMC halt error.
  Suspend needs an error-returning operation and retained recovery state.
- UVD needs an explicit session contract: preserve/rebuild firmware and decoder
  state, or notify clients of loss so they can recreate sessions. Restoring
  frame buffers alone does not establish decoder continuity.

### Implementation order

First add the lifecycle/admission model and timeout/rollback tests without
changing the current sleep veto. Next exercise quiesce and restore with PCI
power retained, checking live GL buffers, surfaces, video, fences and both
heads. Full system sleep follows only after VRAM/PCI restoration is implemented
and tested with the user present. Qualify Cayman and Turks separately, including
both cards installed. Remove the sleep veto only for a validated configuration.

## What happened on the G5

- `pmset displaysleep 1` and 3 minutes idle: the display never slept and the
  kext never logged a power request.
- Apple menu > Sleep: "System Doze", back within seconds, the picture frozen,
  `cgmode list` showing no display, ssh fine. Killing the window server: the new
  one hung in `IOFBRebuild` in the kernel, state `Us`. A shutdown over ssh then
  killed the network but the Mac stayed on with a frozen picture. A hard power
  off was the only way out.

## The cause (found by reading Apple's IOGraphics, checked in Tiger's binary)

The sources used are Apple's, tag `IOGraphics-179`
(`github.com/apple-oss-distributions/IOGraphics`). Tiger 10.4.11 shipped
IOGraphics 179.2.1 (`docs/RESEARCH.md`), so this is the same release line
(tag 179, not 179.2.1: small differences are possible); the tags before it
(123 to 128) lack the wrangler properties the G5's kext has. They are APSL: read to understand, never
copied (CLAUDE.md). Files kept under `build/tmp/iog/` (git-ignored).

1. `IOFramebuffer::extEntry()` is the first thing every call from user space
   into the framebuffer does (`IOFBRebuild`, mode queries, the shared cursor,
   `extSetBounds`, ...). It closes the framebuffer gate and, **while a flag
   (`pagingState`) is clear, sleeps on the gate, uninterruptibly**
   (`sleepGate(&serverConnect, false)`). The disassembly of Tiger's
   `IOFramebuffer::extEntry` (0xb348 in the kext's own addressing) does the same:
   a test of one bit in the framebuffer object, `sleepGate`, test again.
2. System sleep clears it for every framebuffer: `systemPowerChange` ->
   `startThread` -> `sleepWork` -> `checkPowerWork`: the window server is told
   (`notifyServer(false)`), `handleEvent(kIOFBNotifyWillSleep)` is called, the
   flag is cleared, and the system's sleep is acknowledged.
3. Only the way back sets it: `handleEvent(kIOFBNotifyDidPowerOn)` while the
   system is up sets the flag and delivers `kIOFBNotifyDidWake`, and `wakeServerState`
   wakes the gate. That runs when the framebuffer is told to go to its highest
   power state: `IOFramebuffer::setPowerState` queues it and the sleep thread
   calls the driver's `setAttribute(kIOPowerAttribute, state)` and then
   `acknowledgeSetPowerState()`.
4. **`setPowerState` is only called if the framebuffer registered a table of
   power states with power management.** The base class does `PMinit()` and
   `joinPMtree()` but not `registerPowerDriver()`; the subclass must. Apple's own
   reference subclass does (`IONDRVFramebuffer::initForPM`); ours does not (no
   `registerPowerDriver` anywhere in `kext/RadeonNI`). So: nothing ever tells our
   framebuffer to power on, the flag stays clear after the first system sleep, and
   every call from the window server sleeps in `extEntry`, forever and
   uninterruptibly. That is the frozen display, the window server stuck in
   `IOFBRebuild`, the unkillable process, and the shutdown that stops half way
   (loginwindow waits for the window server).
5. It also explains why display sleep never reached us: the wrangler's idle
   timer works through the same power tree.

## What Apple's reference does (`IONDRVFramebuffer`, same tag)

- `initForPM()`: three states: 0 (off: no flags), 1 (doze: input power on,
  output off, not usable), 2 (`IOPMDeviceUsable`, input and output power on).
  `pm_vars->theNumberOfPowerStates = 0; registerPowerDriver(this, states, 3);
  temporaryPowerClampOn(); changePowerStateTo(doze)`. Overrides
  `maxCapabilityForDomainState`, `initialPowerStateForDomainState` (domain on:
  highest state, else sleep) and `powerStateForDomainState` (domain on: current
  state, else sleep).
- When the board cannot be powered off safely (legacy desktops, a driver that
  says it cannot) the states carry `kIOPMPreventSystemSleep`: the system then
  cannot sleep while the framebuffer is in them, only the display can.
- `setAttribute(kIOPowerAttribute, n)` -> `ndrvSetPowerState`: leaving the top
  state: `handleEvent(kIOFBNotifyWillPowerOff)`, sync off (DPMS); entering the
  sleep state: tell the accelerators (`handleEvent(kIOFBNotifyWillSleep, true)`)
  and redirect the VRAM mapping (`getVRAMRange()->redirect(kernel_task, true)`)
  so processes touching the aperture do not touch a dead card; coming back from
  sleep: undo the redirect, `handleEvent(kIOFBNotifyDidWake, true)`, re-read
  whether the display is online (`kConnectionEnable`), re-probe, reset AGP;
  reaching the top state: `handleEvent(kIOFBNotifyWillPowerOn)`, then
  `handleEvent(kIOFBNotifyDidPowerOn)` and the sync back on.
- `setAttribute(kIOSystemPowerAttribute, kIOMessageSystemWillPowerOff/Restart)`:
  quiesce the hardware before the machine goes away.
- `initForPM` is called from the driver's start/enable path, after the base
  `IOFramebuffer::start`.

IODisplay and IODisplayWrangler register their own power states the same way
(`IODisplay.cpp`, `IODisplayWrangler.cpp`).

## Plan for the kext (not started)

1. `RadeonNI::initForPM()` as above, called after `enableController` succeeds.
   Start with `kIOPMPreventSystemSleep` on every state: the card's survival of a
   doze is unknown (the G5 may cut the slot's power; VRAM contents, the GART
   table and the engine's state would all be lost), and with the flag the
   machine only does display sleep and "Sleep" does nothing. Check first on the
   G5: `ioreg -c IOPMrootDomain -l` for the PM features (the reference tests
   `kPMHasLegacyDesktopSleepMask` and `kPMCanPowerOffPCIBusMask`); the G5 logged
   "System Doze", which fits a doze-only desktop.
2. `maxCapabilityForDomainState`, `initialPowerStateForDomainState`,
   `powerStateForDomainState` as in the reference.
3. `setAttribute(kIOPowerAttribute, n)`: move our `setOutputPower` logic into
   the reference's ordering (WillPowerOff before the output goes off,
   WillPowerOn before it is brought back, DidPowerOn after the mode set), and
   never block in it (it runs in the framebuffer's sleep thread holding the
   framebuffer gate).
4. `kIOSystemPowerAttribute` for restart and power off.
5. If system sleep is allowed later: redirect the aperture mapping the
   accelerator gives to processes, `kIOFBNotifyWillSleep/DidWake` with `true`,
   a re-POST and re-init of the engine, GART and surfaces on wake (the whole of
   `rdn_post`, `rdn_gpu`, `rdn_cp`, `rdn_gart` again) and a test with a game
   running.
6. Keep `rdn_dpms=0` as the kill switch (it exists: `fDpms`), and have it also
   skip `registerPowerDriver`.

## Test procedure (the user must be at the G5)

Never a first test of a power path without the user present and a plan to get
out: a hung window server needs the power button. Order: (a) display sleep from
an ExposÃƒÂ© hot corner "Put Display to Sleep" with the kext log read after sleep and
wake; (b) system sleep only after step 5 above; (c) hot-plug during each.
Do not write properties to `IODisplayWrangler` (Tiger panics; journal). Tiger's
`pmset` has no `sleepnow` and no `displaysleepnow`.

## Sources

- IOGraphics (tag 179): `IOGraphicsFamily/IOFramebuffer.cpp` (`extEntry`,
  `checkPowerWork`, `sleepWork`, `setPowerState`, `powerStateWillChangeTo`,
  `systemPowerChange`, `handleEvent`, `extAcknowledgeNotification`),
  `IONDRVSupport/IONDRVFramebuffer.cpp` (`initForPM`, `ndrvSetPowerState`,
  `maxCapabilityForDomainState`), `IOGraphicsFamily/IODisplay.cpp`,
  `IODisplayWrangler.cpp`.
- Tiger's own `IOGraphicsFamily` 1.4.2 on the G5, disassembled with `otool -tV`
  (`IOFramebuffer::extEntry`; the strings of the wrangler's properties).

## Cayman SMC bring-up (2026-10-24, experimental)

The V7900 (1002:6704) SMC port is in `hw/rdn_cayman_dpm.c` and
`hw/rdn_smc.c`, adapted from Linux v6.12 Radeon. It uses the board's
PowerPlay TDP/near-TDP limits, leakage LUT, voltage GPIO LUTs and ATOM PLL
dividers. This ROM specifies 95/50 W and 420 MHz/950 mV through
725 MHz/1040 mV. Active-display memory stays at 1250 MHz with its existing
timings. This is not a system-sleep implementation.

The first automatic-start candidate hung after the SMC acknowledged activation
and the accelerator was published. The recovered boot log confirms that the
memory-clock transition had completed. Hardware thermal
throttling and power containment remain unvalidated. Startup is now disabled
at compile time by default (`CAYMAN_SMC=0`). An experimental build requires
`CAYMAN_SMC=1`; `rdn_dpm=1` or `RDN_UC_POWER_START_SMC` then requests startup. Keep automatic startup disabled until the native thermal,
power-clamping and rendering tests pass. The guarded source builds with the native Leopard compiler but has not yet been
retested on the V7900. The failed candidate was rolled back after recovery with
a 6670.

Recovery boot arguments are `-v rdn_bootclocks=1 rdn_dpm=0`. The boot-clock
argument also skips the static fast-clock transition. Startup without SMC
keeps Cayman at its boot clocks and does not run the software boost governor.
The controlled SMC path uses the previously tested voltage/core/memory BIOS
transition order before handing clock ownership to firmware.

SMC startup checks the firmware checksum, SRAM readback and required mailbox
responses. The intended policy uses an 85 C digital thermal threshold and the
BIOS power limits. A separate 90 C/80 C watchdog requests the lowest SMC state
on overheating or an invalid sensor. Manual `POWER_BOOT` requests the low
state, `POWER_PERFORMANCE` restores automatic selection, and
`POWER_THERMAL_LIMIT` accepts 50..85 C for controlled testing. These requests
retain hardware protection; they do not write static PLL settings while the
SMC owns the clocks.

The on-card SRAM interface and tables use big-endian storage independently of
host byte order. ATOM's exact Q14 feedback dividers reproduce the live PLL;
the upstream 16834 multiplier did not match the board's register readback.
The firmware remains outside Git and is distributed with `LICENSE.radeon`.
Model tests cover transport timeouts, table serialization, calibration,
firmware corruption and failed-start rollback on x86 and big-endian PowerPC.


Turks DDR3 memory-clock startup checks the memory type rather than requiring
GDDR5's MC sequencer RUN bit. The Diamond 6670 BIOS specifies 800/600 MHz;
150 MHz is its memory boot clock, not its performance setting.

`RDN_UC_POWER_TUNE` accepts core MHz in the upper 16 bits and memory MHz in
the lower 16 bits. Currently only device 6758 with DDR3 is eligible, with
both clocks between BIOS performance and 105% of that value. Voltage writes
are excluded. A 30-second lease expires at 75 C or on an invalid sensor;
restoration waits for queued work and retries on failure. This is a bounded
experiment, not a qualified persistent overclock or hardware failsafe.

`gpucardbench` measures offscreen blend, texture and shader throughput with
pixel checks. An optional output prefix saves complete RGBA frames outside
the timed section for stock-versus-tuned comparisons. `gpupowercheck` reads
PCIe status and exports the cached board ROM without touching the ROM BAR.
The high bit of the existing register-read offset selects read-only PCI
configuration space. GPIO VID selection is a nominal voltage request;
without rail telemetry it cannot establish actual voltage or power quality.


Leopard's SPDisplaysReporter converts the 32-bit VRAM byte count as signed,
so 2 GiB appears as -2048 MB. `tools/patch-spdisplays-vram.py` creates a patched
copy of the verified reporter build, replacing those PPC divisions with
unsigned shifts. It refuses other binary versions and never installs its
output. Keep the original reporter backed up when deploying this OS-side fix;
the driver's actual VRAM byte count remains unchanged.

The follow-up handoff audit copies static F0 arbitration timing into F1 before
SMC ownership, gives each driver level its upstream arbitration-table index,
and snapshots the initial PLL state after ATOM programs the idle clock. Display
presence is explicitly reported before global DPM starts. Disable returns to F0
before restoring the saved F1 timing controls. These corrections pass host and
big-endian PowerPC models; they do not establish that the earlier native freeze
is resolved. Keep automatic startup disabled until a Cayman hardware run verifies
transitions, load recovery, and the thermal limit.


The integrated dual-GPU diagnostic kernel at `/Users/shay/dual-smc-20261024`
adds a queued-fence drain before the ATOM performance transition. Startup rejects
active video work, invalid temperature readings and temperatures of 80 C or more.
Its native Leopard build and loadability check pass. Kernel SHA256:
`930c01ccd85c5395aec375e06e3b2f6ee73af4a6b2c1d46e40486116fa106387`.

Manual activation on .98 succeeded with both accelerators present. Cayman changed
from 250/150 MHz to firmware-controlled 420-725/1250 MHz. Sampling under game load
observed 725 MHz and a subsequent return to 420 MHz around 76 C with the thermal
threshold set to 75 C. The runtime threshold was then set to 80 C; temperature
was 79 C with no reported hardware-power fault. CAC and TDP clamping acknowledge
startup, but their power accuracy and independent clamp response are unverified.
Automatic activation remains disabled (`rdn_dpm=0`); this run does not validate
automatic startup, sleep/resume or a sustained thermal soak. The prior kernel
and boot cache are retained as `rollback.kext` and `rollback.mkext` in that directory.


RadeonNI Driver Manager (`tools/guest/gputemp.m`) is a native Leopard application
with a tab per physical GPU. It uses each framebuffer's own accelerator path;
telemetry and power controls cannot silently target the first enumerated card.
It shows live PLL clocks, sampled core/MC activity, installed VRAM, the separate
CPU aperture and allocation limits, framebuffer geometry, power status and GPU
surface-copy counters. Requested voltage and estimated power are explicitly
labeled; actual rail voltage, fan RPM, VRAM usage and UVD activity are unavailable.
The diagnostic report includes both cards. Power controls remain Cayman-only.

The Performance Mode checkbox starts guarded SMC management when necessary,
then requests the highest stock core state and a 90 C thermal limit. Disabling
it clears the forced state and restores automatic core selection with an 80 C
limit. Firmware thermal and power containment remain enabled. The watchdog caps
clocks at the configured limit and restores the requested policy after cooling
5 C below it. Performance Mode holds the high state even at idle; it is not an
automatic boost profile. Cayman memory remains fixed at 1250 MHz after SMC
activation; memory DPM and automatic activation at boot remain disabled.

The .98 build installed in `/Users/shay/radeonni-manager-20261024` has SHA256
`9a1bc7c4608fe9682ab93b4363e586b5bb8e37657e610418b02bae7bb0745d44`.
After reboot, the native UI checkbox selected 90 C and measured 724.99/1250 MHz
at 81.5 C with power containment enabled and no reported SMC error. Disabling it
selected 80 C and measured 419.97/1250 MHz while thermal recovery was active.
The HD 7570 remained at 649.96/800 MHz with its control disabled. These are
transition checks, not a sustained thermal qualification. The DPM model passes
36 checks on x86 and big-endian PowerPC, including 90 C acceptance, rejection of
91 C without writes, forced-state failures and preservation of thermal controls.

Build the app on Leopard with `scripts/build-manager.sh`; the output is
`build/RadeonNI.app`. The deployed copy is `/Users/shay/Desktop/RadeonNI.app`.


The Cayman diagnostic command `RDN_UC_POWER_UNDERVOLT` (7) accepts an exact BIOS
core-voltage step no more than 100 mV below the stock high-state voltage and
never below the BIOS idle VID; zero
restores stock. It restricts the firmware to low, waits for the low-state index,
halts the SMC using the upstream state-switch sequence, verifies the updated high
level in SRAM, and restores CAC/TDP containment before allowing high clocks.
Thermal configuration, memory timing and the idle VID do not change. Failure
attempts to restore the prior table; failed recovery blocks further experiments.

A driver-side 30-second lease restores stock and leaves a manual low-clock cap.
Invalid temperature or reaching the thermal limit also ends the experiment.
This is not a persistent profile or a guarantee of recovery from a GPU hang.
The manager reports the temporary state, current requested VID and last voltage
request separately. No voltage slider is exposed.

Native diagnostic kernel SHA256:
`ff4cd00351496befe304a7655e30145f62044c532194e9053e480bfaa45c8ffd`.
Its prior kernel and boot cache are in `/Users/shay/undervolt-20261024/rollback.kext`
and `rollback.mkext`. The model passes 44 checks on x86 and big-endian PowerPC,
including transient table-reload failure recovery and rejection of unsupported
VIDs. On .98, a 990 mV request held 724.99/1250 MHz, passed short blend, texture
and sine-shader pixel checks, and reached 87 C. This does not establish sustained
stability, measured rail voltage, board power savings or a temperature improvement.
The first offscreen test selected the other renderer and is excluded from Cayman
rendering evidence. The corrected test binds a window to the primary display and
checks renderer 0x1b00 plus owning GLD PCI ID 0x6704; the legacy GL_RENDERER string
still incorrectly names the 7570 and cannot identify the physical device.


The 950 mV diagnostic build (`54002fc6...`) widened the bounded high-state trial
to 100 mV below stock, still requiring an exact BIOS VID. A repeated 640x360
blend/texture/shader run verified the owning FirePro renderer and ran at
724.99/1250 MHz for about 100 seconds before the benchmark's 89 C cutoff.
Completed pixel checks passed. A prior attempt ended on a busy telemetry read;
the test now retries only kIOReturnBusy, and a later run logged one successful
retry. The temperature cutoff, not a rendering error, ended the subsequent run.
No sustained cooling benefit or long-term stability is established.

The three-state revision uses the BIOS low core clock/VID (250 MHz / 900 mV on
V7900), an intermediate 550 MHz / 950 mV state, and the stock 725 MHz / 1040 mV
high state. Memory remains at the existing display clock and I/O voltage in all
three states. Both display watermark sets are recomputed for the minimum core
clock. High-state undervolt trials now modify level 2, not the intermediate level.

At the configured thermal limit minus 5 C, software requests the intermediate
state; at the limit, or with an invalid sensor or power fault, it restricts to
the lowest state. Deep cooling persists until limit minus 5 C, and intermediate
cooling persists until limit minus 10 C. Thus Performance Mode uses 85/90 C
entry thresholds and 85/80 C recovery thresholds. Manual low caps still override
the profile. Performance Mode forces the high state only outside these thermal
stages; Balanced uses automatic firmware selection. The manager exposes the
thermal stage and both reduced clock targets. This does not implement memory DPM.

Native testing rejected the initial arbitrary-level selector implementation.
SetEnabledLevels(2) returned an error; SetForcedLevels(2) acknowledged the request
but did not enforce the intermediate clock. In the third build
(`cac78df121415e6ac491b40ef536352813a375ccd01e17de22af9d869826789e`),
the 725 MHz blend, texture and shader checks passed, but the warm stage remained
at 725 MHz. That build is not validated as a three-stage thermal controller.

The replacement restricts to low, waits for the hardware low-state index,
halts the SMC and changes driverState.levelCount to two before resuming and
selecting the highest remaining level. Recovery restores all three levels.
Failed updates restore the previous header and retain the low cap. Memory
timings, level images and hardware thermal controls remain unchanged.
The model passes 55 checks on x86 and big-endian PowerPC, including serialized
level-count updates and rollback after a failed state expansion.

The first replacement binary (`dc0d7f29...`) was truncated: its Mach-O string
table extended beyond EOF. Leopard's dry-run loadability check missed this, and
both accelerators failed to load at boot. A fresh link passed nm validation;
the build now links to a temporary file and checks its symbol table before
replacing the output. The startup disk had only 8 MiB free during diagnosis.
Old test caches were copied off-host, hash-verified and removed; this is not a
general resolution of the full-disk condition.

The rebuilt kernel
`5ba484f4e42c8119579594df37812e355bc929632795214d3d9ec04d4098f6ac`
booted with both accelerators active. Lowered test thresholds exercised
724.99 -> 549.98 -> 249.99 MHz, with requested VIDs 1040 -> 950 -> 900 mV.
Each state passed the three one-second 640x360 rendering checks. The low state
held for another 20 seconds; restoring the 90 C profile returned to 724.99 MHz.
No HardwarePowerFault or HardwarePowerError was reported. These short checks
validate transitions and sampled pixels, not sustained thermal performance,
measured rail voltage or long-term undervolt stability. Memory remained 1250 MHz.
Automatic SMC startup remains disabled by rdn_dpm=0.
The healthy pre-change rollback remains in
`/Users/shay/three-state-v4-20261024/rollback.kext` and `rollback.mkext`;
the v5 directory's rollback binary is the malformed build and must not be used.


### Board telemetry, 2026-10-25

The running .98 boot now uses `rdn_dpm=1 rdn_irq=0`; automatic SMC startup
and hardware thermal/power containment were verified after reboot.

`RDN_TELEMETRY_BOARD` is a read-only framebuffer query, serialized by the
power lock. Version 1 returns per-sensor validity plus raw tach period,
reference frequency, BIOS pulses/revolution, configured edges and RPM.
Cayman uses the SpeedFanControl bank at 0x788/0x78c, not SI's 0x770/0x774.
The BIOS reference clock and divider determine the conversion. Invalid
periods, unknown clocks and pulse-count mismatches are unavailable readings,
not zero RPM. Turks remains explicitly unimplemented until its bank is
validated. Queries never change PWM, clocks or voltage.

The V7900 ROM declares four pulses/revolution and a 700â€“6600 RPM display
range. Live read-only samples on .98 returned periods 237355â€“237399, with
four configured edges and a 27 MHz reference. These convert to about
6824â€“6825 RPM; the BIOS display maximum is not used to clamp measurements.
The earlier bounded fan test recorded the period falling as PWM increased.
No independent optical tachometer measurement has been performed.

Measured rail voltage is still unimplemented. The core voltage object is
GPIO-controlled (0x80), and voltage requests/VIDs must not be presented as
ADC measurements. The API reports this limitation separately from a missing
BIOS description, and reserves microvolt units for an actual sensor backend.

The new sensor model passes 49 combined PM checks on x86 and big-endian PPC.
The manager and kernel build natively on Leopard. The loadability-checked
candidate is `69af2762e3435ffb108e5aa47a116f255feca9b21374b0dc2ca70e000457cb50`
in `/Users/shay/command-reserve-20261025/telemetry-candidate/RadeonNI.kext`.
It was installed into `/System/Library/Extensions/RadeonNI.kext` with a
validated local-root PPC boot cache. The updated desktop manager was installed
and reopened. The old driver, app and cache are saved in
`/Users/shay/command-reserve-20261025/telemetry-install-backup` and copied
off-host. No reboot was performed; the live kernel remains `d81d9418...`.
The new board-query ABI still needs post-reboot validation.


## PowerPlay coverage audit â€” 2026-10-25

PowerPlay support is partial and differs by ASIC. The V7900 runs a custom
core-only SMC policy with thermal and estimated-power containment. Turks
uses static BIOS performance clocks. Neither path implements the full BIOS
state policy or dynamic memory management.

### Evidence and running configuration

Read-only inspection used both live IORegistry VBIOS images, six passive
samples approximately two seconds apart, direct power-register reads and the
source paths below. Chromium and the user's desktop remained running; no
clock, voltage, temperature-limit, fan or boot settings were changed.

Evidence is in the host directory
`h264-validation-20261022/command-reserve-20261025/`:
`powerplay-live-audit.json`, `powerplay-passive-samples.json`,
`powerplay-registers.json` and `powerplay-live-rom-summary.json`.
The last file also decodes the saved 6670 ROM, explicitly marked offline.

The boot arguments were `-v rdn_irq=0 rdn_dpm=1`. The boot session predates
the telemetry-driver installation. The installed file has SHA-256
`69af2762e3435ffb108e5aa47a116f255feca9b21374b0dc2ca70e000457cb50`;
installing it did not replace the running kernel image. The latest source's
90 C default and 80â€“100 C selector are not installed or active.

| Feature | V7900 / Cayman | 7570 and 6670 / Turks |
| --- | --- | --- |
| Read BIOS clock/voltage states | Yes; selected fields | Yes; selected fields |
| Static BIOS clock/voltage programming | Yes, during SMC handover | Yes; normal performance path |
| SMC firmware and autonomous core/VID scaling | Active on .98 | Not ported |
| Dynamic memory clock / VDDCI states | Disabled; 1250 MHz / 1150 mV VID retained | No DPM; 7570 at static 800 MHz memory |
| Hardware thermal throttling | Enabled on live V7900 | Not enabled by this driver path |
| Software thermal stages | Custom 250/550/725 MHz core policy | No equivalent governor |
| Estimated-power containment | Enabled; BIOS targets and CAC model | Not ported |
| Dynamic fan curve | No; Cayman forced to maximum PWM at POST | No driver fan policy; inherited BIOS configuration |
| BIOS UVD/video state selection | Not integrated | Not integrated |
| Decoder clock shutdown | Separate UVD wake/idle path | Separate UVD wake/idle path |
| ULV / deeper idle power policy | No; ACPI/ULV images clone initial state | No |
| Explicit graphics/memory/light-sleep gating policy | Not ported; inherited BIOS settings are not proof of runtime management | Not ported |
| Dynamic PCIe link policy | Not ported | Not ported |
| Runtime display-change notification to SMC | Startup notification only | No SMC policy |
| Full system suspend / GPU power-loss recovery | Unsupported; driver prevents system sleep | Unsupported |
| Measured rail / board watts | Not available | Not available |

The live V7900 had `GENERAL_PWRMGT=0x7643`, `SCLK_PWRMGT_CNTL=0x000f3802`
and `MCLK_PWRMGT_CNTL=0x0700ffef`: global DPM enabled, thermal protection
not disabled, core DPM enabled, memory DPM disabled. The live 7570 returned
`0x7604`, `0x000f3812`, `0x0700ffcf`: global DPM off and the thermal-protection-
disable bit set. This does not establish whether an independent board/VRM
emergency protection exists.

An initial V7900 query returned 549.98/1250 MHz at 84.5 C, stage 1, with an
85 C limit. The passive sample series then observed stage 2 at
249.99/1250 MHz and 81â€“81.5 C. The five-degree recovery rule retains stage 2
until 80 C at that limit. No hardware-power error or fault was reported.
These observations establish active thermal caps; they do not exclude
simultaneous power-model clamping. The reported 11.8â€“18.3 W estimates are
not measured card consumption.

### BIOS states versus the implemented policy

The live V7900 ROM contains performance levels 250/150 MHz at 900/950 mV,
420/1250 MHz at 950/1150 mV and 725/1250 MHz with a virtual core VID. It
also has separate UVD-class levels at 420 and 650 MHz core. Our preparation
replaces the low memory state with the current 1250 MHz state, retains its
1150 mV I/O VID, and changes the intermediate core target to 550 MHz.
`rdn_pm_resolve_state()` resolves the high virtual core VID to 1040 mV.
Those voltages are programming values, not rail measurements.

The 7570 ROM contains 100/150, 400/800 and 650/800 MHz performance levels.
The saved 6670 ROM contains 100/600, 400/600 and 800/600 MHz. Thus the 6670
ROM supplies core idle savings but no lower memory clock in that state set.
The current Turks path selects the maximum state and does not run a governor.

`rdn_pm_performance_state()` selects the highest non-boot clock; it does not
implement a policy engine over the BIOS classification, caps, voltage-
dependency, video, AC/DC or display restrictions. The existing idle selector
uses the classification's low bit and optional exact memory-clock matching.
These helpers are a subset of PowerPlay parsing, not general BIOS policy.

### Power containment and the 95 W value

The live V7900 PowerPlay header declares TDP 95 W, near-TDP 50 W and a
20 percent TDP adjustment allowance. The driver scales 95/50 into the SMC
power-model units, derives a 40 W safe target and leaves the boost target
zero. It does not implement the BIOS adjustment allowance in a user control.
Raising 95 to 150 would be a 57.9 percent change, exceeding that allowance;
this is not justified by the board's advertised maximum power rating.

CAC uses the upstream Cayman-LE weights, which upstream selects for PCI
6704. The simplified leakage LUT repeats each BIOS leakage entry across
all temperatures; upstream explicitly supports this simplified mode too.
Its zero display/PCIe constants also match the LE reference. They are not,
by themselves, evidence of a broken port. However, this remains a model
using activity weights and BIOS leakage rather than input-rail metering.
No physical calibration or external board-power comparison has been made.

The port always uses LE weights after its Cayman-family/BIOS-format checks.
It has no XT/Pro weight selection for other Cayman boards, so current V7900
qualification must not be treated as broad Cayman PowerPlay qualification.

### Concrete gaps and implementation order

1. **Thermal policy across all supported cards.** Port the BTC/Turks SMC path
   or provide a validated core/voltage fallback. `startIdleGovernor()` currently
   returns for every Cayman card and subsequently rejects every non-Cayman
   card, making the old governor unreachable. Failure/disable of Cayman SMC
   leaves the boot core clock rather than an automatic fallback governor.
2. **Memory DPM.** `memory_snapshot()` copies the current PLL image into each
   level and sets `MPLL_PWRMGT_OFF`; enable explicitly keeps that bit set.
   The dynamic MC-register table, per-level PLL calculation, strobe/DLL/EDC
   policy, per-level arbitration/refresh timings, and display-gap/active-head
   coordination are absent. The current identical arbitration-table entries
   only support the fixed-memory design. Lower MHz labels alone cannot fill
   this gap. Memory downclocking remains unimplemented.
3. **Throttle attribution and calibration.** Expose raw CAC values, requested
   versus actual SMC state, thermal-stage cap, manual cap and power-clamping
   indicators in one timeline. Validate using rendering checks and measured
   external power before changing BIOS TDP targets. The current stage is a
   software thermal classification, not an exhaustive hardware throttle reason.
4. **Power-state integration.** Select video/idle/display states from the BIOS;
   connect UVD activity and display changes to policy; port supported clock-
   gating/light-sleep/ULV paths. The standalone decoder uses fixed 533/400 MHz
   wake clocks and a separate idle/shutdown mechanism, not BIOS UVD-state DPM.
5. **Serialization and lifecycle.** The hardware-power timer holds the
   accelerator lock but does not acquire the framebuffer power lock used by
   `power()` and telemetry clock queries. Audit and unify this lock contract
   before adding memory transitions or modeset-dependent firmware tables.
   This is a code-level concurrency concern, not a reproduced cause of a hang.
   Full suspend requires GPU/VRAM restoration and SMC reinitialization.

References: local `hw/rdn_pm.c`, `hw/rdn_cayman_dpm.c`, `hw/rdn_smc.c`,
`kext/RadeonNI/RadeonNIAccel.cpp`, `kext/RadeonNI/RadeonNI.cpp` and
`hw/rdn_uvd.c`; Linux Radeon v6.12
[`ni_dpm.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/radeon/ni_dpm.c),
[`btc_dpm.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/radeon/btc_dpm.c),
and [`cypress_dpm.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/radeon/cypress_dpm.c).


### Coordinated PowerPlay candidate (2026-10-25)

The preceding audit describes the installed driver before this candidate.
The new implementation is Cayman-specific and has not been boot-qualified.
Turks/7570 memory DPM and full system suspend remain outside this change.

The candidate derives performance and video levels from the V7900 BIOS:
250/150, 420/1250 and 725/1250 MHz core/memory. It generates memory PLL,
strobe/DLL/EDC settings, MC register tables and arbitration/refresh timings.
Memory transitions require one active head with at least 460 microseconds
of blanking, or no active heads. Multiple unsynchronized heads on the same
GPU retain high memory clocks. Modesets drain outstanding work and pin
memory high while display geometry changes. The initial arbitration slot
retains the stable 250/1250 MHz handover timing.

Decoder wake selects the BIOS video state before starting UVD and uses its
540/400 MHz VCLK/DCLK. Idle policy requests the BIOS low state after two
seconds without submitted work. Upstream Cayman graphics clock gating,
light sleep and medium-grain clock gating are included. This board has no
BIOS ULV state; the driver does not invent one or lower voltage below BIOS
levels. The BIOS 95 W SMC target is unchanged.

The manager accepts thermal limits from 80 to 100 C, defaulting to 90 C,
with five-degree recovery hysteresis. Thermal caps use BIOS 420 and 250 MHz
core levels. Dynamic fan control uses a bounded BIOS curve when available,
otherwise 60 percent through 50 C, 80 percent at 70 C and full fan at 80 C.
Fan reductions are rate limited; invalid temperature, sustained invalid tach
or PWM readback failure requests full fan and caps performance.

The UVD staging reservation fix is included in this candidate: accelerator
startup reserves 4,239,360 bytes in the decoder-visible segment before
client allocations. Session cleanup retains this block for reuse, avoiding
fragmentation-dependent decoder startup failures. The native upload list
explicitly includes RadeonNIUvdSession.cpp so an older native source cannot
silently omit the reservation implementation.

Validation: 149 Cayman DPM model checks pass on x86 and big-endian PPC;
clock-gating restore and 49 virtual-voltage checks pass on both. All 26 UVD
session tests pass, including fragmentation and reservation reuse. Guarded
UVD and legacy UVD models pass on both architectures. A real V7900 ROM
preview reproduces the live 1250 MHz memory PLL image exactly. These checks
cover software contracts and firmware table generation, not physical
memory stability, display continuity, fan response or measured power.
Native build evidence and candidate manifests are kept outside the source
tree in command-reserve-20261025. Installation and reboot validation are
still pending. Changes remain unstaged.


#### First boot and recovery policy

The first accelerated boot exposed a hardware failure not covered by the
models: the V7900 entered 150 MHz memory, then the SMC stopped responding
to SetForcedLevels (mailbox 0x83 without a response). Core readback was
419.97 MHz. HardwarePowerError was -110 and the fault latch rejected later
submissions. Both accelerators existed, but rendering was not healthy.
The new memory transitions and graphics gating have not passed hardware
qualification; their individual contributions are not yet isolated.

Recovery keeps memory at the BIOS performance clock and I/O voltage.
Memory DPM now requires rdn_memdpm=1, and the new graphics gating sequences
require rdn_gfxcg=1. Neither is enabled by default. Core DPM, hardware thermal
protection, BIOS video-state selection, fan control and the UVD reservation
remain included. The memory-register table is now generated after applying
the memory floor, so fixed-high-memory levels cannot retain low-clock MC
rows. All 162 recovery/DPM checks pass on x86 and big-endian PPC.

Native packaging must preserve the complete installed firmware-bearing
personalities; the source default plist does not enable acceleration or
embed firmware. The earlier telemetry candidate also had this packaging
omission, hidden by its preexisting boot cache. Installation now restores
the complete personalities, forces the boot archive rebuild, and moves
stale prelinked caches to the external build directory before reboot.


The first fixed-clock recovery still timed out at the idle transition.
It pinned clock values but left firmware memory PLL control enabled.
Fixed-memory mode now sets MPLL_PWRMGT_OFF in both the live control register
and each serialized level. A model check verifies that bit. The initial
healthy boot snapshot must not be treated as a stability pass: the error
only appeared after idle policy ran. Further physical validation is pending.


The fixed-memory control-bit correction did not eliminate the idle fault:
readback was 419.97/1250 MHz with mailbox 0x83 unanswered. This rules out
memory downclocking or the new graphics gating as necessary triggers.
The low/automatic cap helper now follows ni_restrict_performance_levels_before_switch:
NoForcedLevel, then SetEnabledLevels with the requested mask. Previously it
sent SetForcedLevels(0), which was the observed blocking command. A model
regression rejects SetForcedLevels and verifies that the low cap still
succeeds. Native validation of this command change is still required.


NoForcedLevel allowed pre-login idle, but the post-login wake/idle cycle
still failed, with mailbox 0x41 unanswered. The command substitution alone
is not a verified fix. Fixed-memory recovery now uses ACIndex 0 for every
core level, uploads zero dynamic MC-register entries, and retains the
existing arbitration timings without executing DynamicMemorySettings.
This removes memory-controller reprogramming from fixed-memory core
transitions. The memory DPM diagnostic path retains per-level generation.


The post-login fault also survived removal of dynamic MC tables. Audit of
r600_display_gap found an incorrect literal in display_gap(): 2 means
WATERMARK, not VBLANK_OR_WM (0). Inactive display channels were left at 0
instead of IGNORE (3). The corrected image uses vblank-or-watermark for the
active core channel, vblank (1) for enabled memory transitions, and ignore
for unused channels or disabled memory transitions. Direct register checks
now cover both active and inactive cases. There are 159 passing model
checks per architecture; physical idle/wake validation remains separate.


The display-gap correction was necessary but did not resolve the live
post-login timeout. Recovery now restores the previously deployed
250/550/725 MHz fixed-memory core profile and snapshots the live memory PLL
image for every level. Explicit two-second idle caps are disabled unless
memory-DPM diagnostics are opted in; ordinary SMC automatic core policy
remains enabled. BIOS video selection, thermal control and fan handling
remain present. This is containment while deeper idle/memory transitions
are investigated, not completed hardware qualification of those features.


Recovery's first thermal-stage change still failed while rebuilding an
unchanged non-video context. Context thermal identity now distinguishes
only critical video cooling; ordinary non-video stage changes leave the
firmware tables untouched and use the cap policy. Fixed-memory recovery
also retains displayWatermark 0 at every core level, matching the earlier
implementation. Tests verify both invariants and synchronize the cached
idle/intermediate descriptions with the final memory floor.


Recovery build 600a692460d0d03bb162681fc9b29ad1152d40a70070852cac1a53c74855f330
booted with both accelerators active. The post-login thermal-stage-1 path
held 549.98/1250 MHz with HardwarePowerError, PowerPlayContextError and
FanControlError all zero. Temperature was 83-84.5 C, fan tach about 5146 RPM.
A profile-auto request succeeded and the SMC returned an acknowledged
mailbox response. The V7900 backing-surface ownership/import/resize test
reported zero failures and zero mismatched pixels. The secondary backing
test was not run successfully: launchctl bsexec rejected the GUI bootstrap
operation. Both physical telemetry connections passed. One passive SSH
connection reset; reconnect showed the same boot and no driver fault.

This is a short recovery validation, not a soak-test or qualification of
new memory DPM, graphics gating or forced idle caps. Those remain off by
default. The permanent UVD staging reservation is present in the verified
installed binary. No changes were staged or committed.


### Extended memory-DPM run: desktop stall

The corrected memory-only build fd310631119a599a7c1064cc31fd318ca3ae45e07e61a7c60e4b52f9a0300198
passed low/load clock transitions (250/150 to 420/1250 MHz while thermally
capped), a 20-second two-display color/readback run, and short passive
monitoring. The earlier gating-only build also passed its two-display run.
These are correctness checks, not completed stability qualification.

Later in the memory-only session, WindowServer stopped accepting new GUI
clients. Its main thread was waiting in CARenderDecoderNew through
semaphore_wait_signal_trap. A newly launched Cocoa client was waiting in
_CGSGetCoreGraphicsServerVersion. PowerPlay, fan and display error fields
remained zero. Restarting Aurora's GPU subprocess did not restore the GUI.
The cause of this Core Animation stall is unresolved; it must not be
represented as a proven memory-DPM or clock-gating defect, or as unrelated
to those changes without further evidence.

Recovery restores binary 600a692460d0d03bb162681fc9b29ad1152d40a70070852cac1a53c74855f330
and boot arguments -v rdn_irq=0 rdn_dpm=1. Memory DPM, new clock gating and
forced idle remain disabled. Finder Open on work is unfinished: its
CFPlugIn menu harness enumerates both GPUs, and the launch helper is staged
on the Mac, but the Finder plug-in has not been installed. Launcher context
tests performed while WindowServer was blocked are inconclusive.

### Oct 25 GLD architecture recovery

The installed four GLD executables were PPC64-only. Restored each matching PPC32 slice from the earlier universal build, preserving the currently installed PPC64 slice byte-for-byte. After reboot, WindowServer GPU surface copies resumed. A PPC32 7570 test and PPC64 V7900 test each rendered on both displays with correct color readback for 20 seconds (1143 and 1438 frames respectively). Both devices completed 1000 no-op submission/fence waits (22.922 and 24.594 ms total); these measure queue progress, not game performance. No new timeout/fault was found in the post-boot kernel buffer. This repairs software fallback; it does not establish that the earlier queue stall is permanently resolved. Recovery kernel and conservative boot flags remain unchanged. Evidence: command-reserve-20261025/universal-repair.
