# Plan

## Evergreen and Aruba expansion plan (2026-10-26)

Planning only. No new family is implemented or hardware-qualified by this
section. Keep the existing Turks/Cayman qualification work independent.

### Scope and order

| Family | Initial target | Required distinction |
|---|---|---|
| Redwood | HD 5550/5570/5670 desktop variants | Evergreen VLIW5, DCE4, 6 CRTCs; first shared Evergreen bring-up target |
| Cedar | HD 5450 desktop variants | Evergreen VLIW5, DCE4, 4 CRTCs; no vertex cache and different thread allocation |
| Juniper | HD 5750/5770 desktop variants | Evergreen VLIW5, DCE4, 6 CRTCs; own engine geometry and firmware |
| Cypress | HD 5850/5870 desktop variants | Evergreen VLIW5, DCE4, 6 CRTCs; larger engine/backend configuration and own firmware |
| Aruba | Trinity/Richland integrated graphics | VLIW4, DCE6.1, 4 CRTCs; integrated memory and platform dependencies |

Preferred order is Redwood, Cedar, Juniper, Cypress, then Aruba research.
The first available physical Evergreen card may change the order. Marketing
names are examples; match upstream PCI IDs and distinguish mobile, integrated
and dual-GPU devices. Do not include Hemlock dual-GPU board qualification in
the initial Cypress milestone.

Aruba is not a discrete PCIe Power Mac upgrade. Define the intended host or
emulation environment before implementing platform initialization. Linux's
Trinity callbacks reuse Cayman routines, but this does not establish that our
discrete Cayman memory, POST or power path works on an APU.

### Implementation stages

1. **Explicit family capabilities.** Extend `hw/rdn_device_ids.h` with upstream
   identities and a descriptor for graphics/display generation, CRTC limits,
   memory model and firmware selection. Replace two-way Cayman/Turks choices
   in `rdn_gpu.c`, `rdn_cp.c`, `rdn_mc.c`, the Mesa winsys and profile generator.
   Unknown or incomplete configurations must not fall through to Turks.
2. **Evergreen framebuffer.** Audit `rdn_post.c`, `rdn_modeset.c`, watermarks,
   cursor/LUT and interrupt registers against DCE4. Port generation-specific
   PLL, DIG allocation and AtomBIOS command revisions. Reuse BIOS connector
   discovery and transmitter validation where applicable. Start with one
   digital output and CPU scanout, then modesets and additional outputs.
   Test warm handoff and cold BIOS POST separately. OpenFirmware boot-console
   support remains a separate task.
3. **Memory and engines.** Port each Evergreen case from Linux
   `evergreen_gpu_init`, golden register setup and startup ordering. Preserve
   fuse-disabled units and actual VRAM geometry. Validate each family's PFP,
   ME, RLC and any required MC firmware, including byte order. Bring up GART,
   ring scratch, IB completion and fences before blits or shaders. Do not
   reuse Turks MC training, clock tables or board-specific tuning.
4. **Mesa and OS X acceleration.** Publish the actual Mesa chip family,
   graphics level, tiling/backend configuration and qualified capabilities.
   Check buffer/texture readback before native OpenGL, then GLD, QE/CI and
   application workloads. Keep the existing winsys performance improvements
   where their hardware assumptions hold. Advertise features from verified
   capabilities, without promising an OpenGL version from the family name.
5. **Video and power.** Add family-qualified decode firmware, engine setup
   and codec limits after graphics stability. Evergreen UVD is not the current
   Northern Islands UVD path. Qualify display sleep, system sleep, reset and
   thermal behavior independently; retain BIOS clocks initially.
6. **Aruba platform track.** Audit integrated/stolen-memory discovery,
   aperture/GART coherency, platform firmware data, SMU interactions, DCE6.1
   and bridge-based outputs against the Trinity path. Only after a viable host
   and memory model exist, adapt Cayman-derived rendering with Aruba-specific
   firmware/configuration. Leave UVD/VCE unavailable until separately ported.

### Acceptance gates

- Host model tests: complete PCI classification, no unimplemented-engine I/O,
  DCE register/CRTC bounds, AtomBIOS revisions, firmware rejection and startup
  failure unwinding. Run endian-sensitive cases on big-endian PPC.
- Build both PPC32 and PPC64 driver/Mesa paths and generated personalities.
  Re-run existing Turks/Cayman and Barts/Caicos model regressions.
- Physical qualification per family: cold boot, warm handoff, EDID/modesets,
  framebuffer pixels, sustained ring/fence completion, memory pressure,
  rendering readbacks, WindowServer/application load and recovery. Exercise
  each advertised connector and qualify Tiger/Leopard separately.
- Update the compatibility matrix by achieved stage: source/model checked,
  framebuffer hardware tested, accelerated hardware tested, then video/power.
  Models and successful builds do not constitute a hardware pass.

References: Linux v6.12
[`radeon_asic.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/radeon/radeon_asic.c),
[`evergreen.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/radeon/evergreen.c),
[`ni.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/radeon/ni.c),
[`drm_pciids.h`](https://github.com/torvalds/linux/blob/v6.12/include/drm/drm_pciids.h),
and the X.Org [Radeon hardware tables](https://www.x.org/RadeonFeature/).

## Active roadmap: Leopard stability and family support (2026-10-18)

This roadmap supersedes the historical milestone claims below for Leopard.
The HD 7570 still hangs in Portal. Neither the fence-publication correction,
client-close drain, nor disabling glthread resolved the gameplay hang.

Primary qualification target: HD 7570/Turks, PowerMac11,2, PCIe x16,
Leopard 10.5.8. Stable requires hardware QE/CI, native application OpenGL,
dual-link DVI and DisplayPort, two displays, hotplug, display sleep, and
system sleep/wake. Tiger has a separate qualification track.

| Gate | Required work | State |
|---|---|---|
| A: Portal cause | Preserve failing baseline; correlate submissions, resources and fences; reduce failure; prove correction with a regression | In progress; cause unknown |
| B: ownership | Kernel buffer handles and shared references; validated relocations; completion-based retirement; VRAM/GART residency and eviction | Planned |
| C: Leopard integration | Explicit drawable/pbuffer ownership; tested ABI and entry points; actual GPU QE/CI; remove timing-dependent activation | Planned |
| D: displays/power | Card service and per-display framebuffers; VBIOS routing; coordinated modesets; hotplug, cursor, vblank and sleep/wake | Planned |
| E: recovery | Bounded fault capture/reset; failed fences and reset generations; explicit context invalidation | Planned |
| F: qualification | Game regressions, performance measurements, additional ASICs and modern GL features | Planned |

### Open, noted, not started

- **Leopard: Finder window corrupts while it is moved (2026-10-22, user).** Window title bar and toolbar
  strip show stacked stale copies, the status text smears along the bottom edge; the window body, sidebar,
  desktop, menu bar and Dock are right. No GPU errors in `system.log`. Installed Leopard kext was md5
  `00500aa3…` (Oct 22 14:51), which predates `efc12f7` (`setIDMode` lets surface 1 through the SurfaceGate
  only with BackingSurfaces); suspect the surface/shape path first, unproven. Evidence: the user's screenshot
  `~/Desktop/Picture 1.png` on the G5's Leopard volume, copy in `images/leopard-finder-corruption-20261022.png`
  on arch-server. `scripts/mac.sh g5 grab` does not work on Leopard (the Tiger-built `~/gl/rdnuc grab` returns
  nothing), so look at a user's screenshot or fix the grab first. Start after the Sims 2 investigation.

### Upcoming upstream ports and performance work (2026-10-25)

Source audit, not implemented or hardware-qualified. Linux Radeon was reviewed
at `21a7f44fd0765fcc5194130b28767786eba1d1e7`, including the latest 100
Radeon-directory commits. Mesa inputs are the official 26.2.4 archive (SHA256
`bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9`).
Review covers suspend/resume, rings, DMA, fences, synchronization, memory
placement, r600, the DRM winsys, shader caching and application configuration;
it is not an exhaustive review of every Linux/Mesa change.

#### System sleep: port the upstream lifecycle

- [ ] **UP-01: Port hardware suspend/resume.** Use
  [Cayman `cayman_suspend/resume`](https://github.com/torvalds/linux/blob/21a7f44fd0765fcc5194130b28767786eba1d1e7/drivers/gpu/drm/radeon/ni.c)
  and [Turks `evergreen_suspend/resume`](https://github.com/torvalds/linux/blob/21a7f44fd0765fcc5194130b28767786eba1d1e7/drivers/gpu/drm/radeon/evergreen.c).
  Adapt the register/firmware ordering into `hw/`; do not import Linux DRM
  lifecycle objects into IOKit. Preserve per-family differences.
- [ ] **UP-02: Preserve memory and clients across sleep.** Follow
  [`radeon_suspend_kms/resume_kms`](https://github.com/torvalds/linux/blob/21a7f44fd0765fcc5194130b28767786eba1d1e7/drivers/gpu/drm/radeon/radeon_device.c)
  for eviction, fence drains, BIOS scratch and PCI restoration. Adapt to our
  live BO handles, hidden VRAM, GART bindings, surfaces and video sessions.
  Existing `retire/startEngine` destroys or resets this state and is unsuitable.
- [ ] **UP-03: Connect IOGraphics power transitions.** One coordinator per
  physical card, shared across its heads; gate submissions/timers, restore
  hardware and mappings before wake acknowledgement, and retain the current
  sleep veto until qualification. See `POWER-MANAGEMENT.md`, implementation
  audit, for failure ordering and staged tests. Test powered quiesce/restore
  before real sleep, then both GPU families and a dual-card configuration.

#### Performance candidates, in investigation order

| ID | Priority | Work and upstream basis | Local evidence / required proof |
|---|---|---|---|
| UP-04 | High | Restore r600 disk shader caching (`r600_pipe_common.c`, `util/disk_cache*`). | Both Leopard build scripts explicitly use `-Dshader-cache=disabled`. Establish a Mach-O-compatible function/build identity and stable cache directory; key by compiler build, GPU and relevant options. Measure cold/warm launches and compilation spikes; this is a stutter/startup candidate, not a promised steady-FPS gain. Test invalidation, concurrent writers and corrupt entries on PPC32/PPC64. |
| UP-05 | High | Profile and reduce presentation serialization using upstream per-resource fence ownership and deferred retirement. | Jak traces identify presentation as a major cost. Local queued-surface and async-fullscreen work already exists; finish its qualification instead of duplicating it. Linux KMS page flipping is not a replacement for Leopard CGS surface flushing. Measure GLD completion, CGS IPC and compositor work separately; retain all producer/consumer fences and resize/occlusion correctness. |
| UP-06 | High | Port small-BO slab allocation/reclamation from Mesa `radeon_drm_bo.c` and its submission tracking. | The custom winsys caches whole allocations but has no BO slabs, and rounds small allocations to 4 KB. Add suballocation only with checked relocation ranges and per-entry completion/ownership. Measure allocation/IOKit-call counts, fragmentation and frame-time tails under small-buffer churn. |
| UP-07 | High | Audit residency/heap reporting against upstream `radeon_drm_winsys.c` and `radeon_ttm.c`. | `rdn_winsys_create` still reports GART size as visible VRAM and caps `max_heap_size_kb` to that region despite GART/hidden-memory support. Replace placeholders with actual budgets and per-allocation constraints after the managed-memory work is reconciled. Prove fewer pressure flushes without overcommitting pinned or CPU-visible memory. |
| UP-08 | Medium | Port asynchronous DMA-ring support (`ni_dma.c`, `r600_dma.c`, ASIC callbacks). | Our CP-DMA copies already exist; Mesa currently advertises zero SDMA queues. Cayman has two async DMA engines; Turks requires its own upstream path. Add ring/fence/interrupt, tiling and endian tests before exposing a queue. Benchmark upload/copy overlap against CP-DMA; small transfers may not benefit. |
| UP-09 | Medium | Replace applicable CPU cross-engine waits with upstream-style GPU dependencies (`radeon_sync.c`, `radeon_semaphore.c`). | `rdn_cs_wait_dependencies` blocks the CPU for graphics/video dependencies. Requires real per-engine fence ownership and reset handling; prioritize concurrent video/compositor and future DMA workloads. It is not a reason to remove same-resource synchronization. |
| UP-10 | Medium | Compare ring-pointer writeback and submission batching with upstream Radeon rings/DRM winsys. | `rdn_ring_free` and IB admission read `CP_RB_RPTR` over MMIO. Compare a coherent system-memory writeback path, retaining a verified MMIO fallback. Measure PCIe register reads and CPU submission cost; validate PPC cache ordering, endian layout, wrap and reset. Existing command-pool/reserve work must be reconciled first. |
| UP-11 | Qualification | Qualify the existing Cayman DPM/SMC port and Turks power policy under real game workloads. | This work already exists. Compare effective clocks and thermal/power limits with upstream PM behavior; distinguish thermal limits from CPU/presentation stalls. Preserve hardware protection and use sustained-load recovery tests. No higher-clock or thermal-limit change is implied by this audit. |

#### UP-04 through UP-09 qualification (2026-10-25)

- **UP-04:** Both Leopard architectures now build with the 64 MB disk shader
  cache enabled. Mach-O UUID and CPU type identify the loaded compiler; r600
  includes its debug options in the key. Native PPC32/PPC64 tests on Cayman and
  Turks verify 32 shader outputs, cold/warm reuse, new entries after rebuilding,
  option invalidation and recovery from truncated entries. Concurrent cold
  writers also pass on Cayman in both architectures. Final warm samples were
  101–148 ms versus 217–431 ms cold under concurrent desktop/video load.
- **UP-05:** Direct swap submits drawing and the presentation copy together.
  Stable stored surfaces avoid redundant drawable rebinding; resize, changed
  screen geometry, direct-screen rendering and WindowServer retain rebinding.
  Producer/consumer completion is unchanged. Five unobscured OpenMark runs per
  card improved median throughput from 797 to 915 FPS on Cayman and 794 to
  878 FPS on Turks. Median block p99 frame times changed from 1.508 to 1.324 ms
  and 1.570 to 1.401 ms respectively. YouTube remained running; Cayman clocks
  varied between 250 and 550 MHz, so these are desktop observations, not an
  isolated GPU comparison. `RDN_PRESENT_PROFILE=1` separates binding, swap,
  explicit finish and CGS costs. Migration/state and resize pixel tests pass.
- **UP-06:** `RDN_SMALL_BO_SLABS=1` enables private visible-VRAM allocations up to
  4 KB in 64 KB parents. Shared/NO_SUBALLOC requests cannot reuse slab entries;
  existing per-entry fences gate cache reuse and parent release. The host test
  packs 257 entries into two allocations. Native 1024-buffer, eight-round
  data checks pass on both cards and architectures; traces show five parents
  for over a thousand live buffers. This remains opt-in: OpenMark measurements
  with slabs had variable WindowServer load and do not establish a speedup.
  A final off/on/on/off check gave Cayman 982/981/834/834 FPS and Turks
  951/899/813/813 FPS; the workload shift persisted with slabs disabled.
  Existing GART chunk suballocation is separate from this addition.
- **UP-07:** Heap limits use the device's visible and hidden allocation pools
  plus the capped GART budget. Native output: Cayman 2014 MB total/222 MB visible,
  Turks 990 MB total/222 MB visible, and 512 MB GART each. Local winsys accounting
  separates VRAM/GART, cached storage, host imports and evicted residency; it
  does not measure global occupancy across other clients or slab-parent slack.
  CGL reads physical memory from the matched GPU's registry instead of reporting
  256 MB for every card. Its legacy signed byte field saturates at INT32_MAX
  for 2 GB; the Mesa heap uses the full allocatable capacity. Native PPC32/PPC64
  CGL tests report positive values for both cards. A forced 24 MB residency
  budget test on each card verified 64 MiB of textures through 35 evictions,
  12 promotions, GPU sampling/writes and reuse. Larger per-allocation pressure
  and global residency reporting still need broader qualification.
- **UP-08/09 remain incomplete:** `hw/rdn_dma.c` ports the Turks/Cayman ring
  setup, five-word linear-copy packets, high addresses, fence/trap packets and
  CP/DMA semaphore emission from Linux revision
  `21a7f44fd0765fcc5194130b28767786eba1d1e7`. Fences carry ring ownership and an
  epoch; quiescence requires completion and engine idle before storage release.
  Host and native PPC32/PPC64 packet models pass, and the Leopard kernel links
  the module. No DMA queue is advertised or started. Kernel ring allocation,
  client/resource lifetime, reset/IRQ integration, tiling/endian hardware tests,
  copy-overlap benchmarks and winsys dependency submission remain outstanding.
  Existing CPU cross-engine waits are still active. Do not treat the packet
  model or the OpenMark improvement as hardware DMA qualification.

Evidence is in the build workspace's
`h264-validation-20261022/command-reserve-20261025`: OpenMark logs and metadata,
`openmark-optimization-summary.json`, `cache-final-qualification.json`,
`small-slabs-native-validation.json`, `small-slabs-allocation-stats.json`,
`memory-budget-native.json`, and the native kernel build log. Changes are not
staged or committed.

#### Game profiles and shader paths

- [ ] **UP-12: Load applicable upstream application options in the custom
  frontend.** Both Leopard builds disable `xmlconfig`; `OSMesaCreateContextAttribs`
  sets only four explicit options and does not load the common upstream
  `src/util/00-mesa-defaults.conf` profiles. Add native executable/bundle/version
  matching and map only options supported by our frontend. Keep process-specific
  rules and log the selected profile; do not relax shader correctness globally.
- [ ] **UP-13: Validate profile candidates against the actual Mac renderer.**
  Upstream examples include Savage 2's GLSL line-continuation workaround and
  Unigine's shader/extension compatibility options. Match the same affected
  shader path before adopting them. The Windows Doom 3 BFG rules do not establish
  a fix for original Mac Doom 3; TF2 Linux64, Wine, Vulkan/WSI and modern GCN/RDNA
  rules are not automatically relevant to native PPC OpenGL.
- [ ] **UP-14: Benchmark existing NIR/SB, HyperZ and glthread paths by game.**
  These are already in Mesa/the port, not missing upstream implementations.
  Preserve our id Tech 3/4 and Unreal Engine 2 profile decisions until new
  measurements support a change. Check shader compile time, generated code,
  depth-heavy scenes, client-array uploads, blend correctness and frame-time
  tails. In the inspected 26.2.4 source `R600_DEBUG=precompile` aliases shader-db
  diagnostics; do not treat that name as a production shader-cache optimization.

#### Recent fixes reviewed; avoid duplicate or incompatible backports

- Linux [`f59ad4cca219`](https://github.com/torvalds/linux/commit/f59ad4cca219)
  restores hardware polling in fence-signaled checks to avoid delayed completion
  and sync-heavy performance regressions. Our `rdn_number_wait` zero-timeout
  path already calls `fence_done`, and `rdn_fence_done` reads the GPU counter.
  Preserve this behavior and regression-test missed/delayed interrupts; there
  is no demonstrated missing equivalent to cherry-pick.
- Linux [`617562bbe12d`](https://github.com/torvalds/linux/commit/617562bbe12d)
  corrects MSI address limits using the device DMA mask. Our current fence path
  uses legacy INTx, so this is a requirement for a future MSI port, not a direct
  performance fix for the installed path.
- Linux [`82f1d6042611`](https://github.com/torvalds/linux/commit/82f1d6042611)
  frees ring-backup storage after restore-lock failure. Carry its failure-path
  invariant into sleep/reset ring preservation; our current code has no matching
  `radeon_ring_restore` implementation to patch directly.
- CP-DMA, whole-BO caching, fence interrupts, glthread and its measured 64 KB
  batch adjustment are already present locally. HyperZ is enabled by default
  upstream. Re-measure these paths; do not list them as newly imported features.

Acceptance for each performance item: a source/patch identity, affected ASIC/OS,
reduced correctness test, five comparable runs with median/p95/p99 frame times,
and an uninstrumented game/desktop regression run. No speedup is established by
this source audit. Source copies, commit metadata and checksums are under
`F:/radeonni-leopard-build/h264-validation-20261022/upstream-performance-audit-20261025/`.

### Implementation contracts

- Preserve the failing baseline while investigating gate A; do not use a
  broad refactor to declare its cause resolved.
- Add a versioned v2 user-client interface: actual capabilities, buffer
  handles/generations, immutable submissions with access lists/relocations,
  explicit fence results, retained shared surfaces and display generations.
- Use the existing r600 relocation path and MIT-licensed Evergreen command
  validation. Keep OS adapters separate from freestanding hardware code.
- Retire memory only after every CPU, GPU and imported-surface reference
  ends. Replace the six-pbuffer delay and immediate surface-store frees.
- A failed wait never permits reuse. Recovery failures return bounded
  errors; unexpected GPU resets fail qualification.
- Expand from Turks to Juniper (HD 5750), then Redwood, Cedar, Cypress,
  Barts and Caicos. PCI-ID recognition alone does not establish support.
- Cayman/VLIW4, integrated and multi-GPU configurations, video decoding and
  display audio are separate projects. Modern GL follows native stability.
- Profiles may select measured performance choices; correctness must not
  depend on disabling threading or globally serializing draws.

### Stable-release gates

- Demonstrated pre-fix failure and post-fix success for the reduced hang.
- 1,000 memory/submission/shared-surface/client-close cycles, including
  injected allocation and wait failures, without premature reuse.
- All applicable advertised-feature tests pass; fix harness failures and
  report unsupported features separately.
- Actual cursor dragging, menus, transparency, Expose, Dashboard, CI filters,
  concurrent GL applications, fullscreen changes and display migration.
- Three two-hour Portal sessions and 24 hours of mixed desktop/game/memory
  pressure with no unexpected hangs, resets, corruption or driver crashes.
- 100 mode switches, 50 hotplug/display-sleep cycles, 25 system sleep/wake
  cycles, independent and mirrored displays, and physical 2560x1600 checks.
- Ten cold boots and ten restarts in x16, plus install/upgrade/rollback tests.
- Five benchmark runs, median/tail frame times, investigation of repeatable
  regressions above five percent. Final qualification runs without tracing.

Record build hashes, OS, PCI/subsystem IDs, firmware hash, monitor/link,
settings, duration and diagnostic configuration with every result. Untested
hardware stays unqualified. Ship matched kernel/GL/accelerator packages with
rollback. Resume upstream preparation after gate A; staging and commits
require explicit approval.

### Submission diagnostics

`RDN_CAPTURE_DIR` names an existing directory for optional winsys captures.
Each process/winsys retains sixteen slots containing host-endian command
words, buffer IDs, placements, ranges, access flags, prior fences and the
submission result. Slots with unfinished fences are not overwritten.
This adds file I/O under the submission lock and can perturb timing; it is
not a performance configuration or evidence of uninstrumented stability.

Decode with `python tools/diagnostics/rdn_ib.py --capture CAPTURE.bin`.
Raw saved PPC IBs use the default big-endian mode. `--memory aperture.bin`
also decodes captured indices; stale descriptors and shader usage still need
independent analysis. Foreign aperture handles do not establish per-surface
allocation bounds. This recorder does not yet capture resource contents or
cross-process allocation/free events.

With captured aperture bytes, the decoder also checks logical attribute
ranges for straight-line fetch shaders against their vertex descriptors.
It accounts for base vertex and known attribute widths. Unsupported index
limits, primitive restart, computed indices and missing bytes remain
unchecked. These checks do not establish allocation lifetime, texture or
constant-buffer validity, or GPU completion.

`RDN_CAPTURE_MARKERS=1`, together with `RDN_CAPTURE_DIR`, inserts GPU memory
writes after draw packets. The counter lives in the retained IB allocation;
the capture header records its aperture offset. Read its four bytes as
little-endian. It reports CP progress, not completion of the preceding draw.
This diagnostic preserves asynchronous submission but changes GPU timing.

## Historical Tiger milestones

Phase 1 ends when Mac OS X 10.4 Tiger, running in QEMU, shows its desktop at
native resolution on a monitor connected to the real Radeon HD 7570.

A milestone is not done until the user has confirmed what is on the screen
wherever its criterion involves video output.

**Phase 1 is complete (2026-10-04).** The sections "Known gaps" and
"Deferred to the real G5" at the end list what was left out on purpose.

| # | Milestone | State |
|---|---|---|
| 0 | Know the card, capture ground truth | **Done 2026-10-04** (see REFERENCE-TRACE.md, HARDWARE.md) |
| 1 | Tiger in QEMU with the card passed through | **Done 2026-10-04**: `IOPCIDevice` with BAR0/BAR2 assigned; a probe kext reads the registers through BAR2 (needs the QEMU PCI-hole patch) |
| 2 | Cold POST and modeset from Linux userspace | **Done 2026-10-04**: test pattern at 1366x768 from an un-POSTed card, confirmed on screen by the user |
| 3 | `IOFramebuffer` kext in Tiger on QEMU | **Done 2026-10-04**: desktop on the 7570 at 1366x768, resolution change from System Preferences to 1920x1080, depth switching and cursor, confirmed on screen by the user |

## Needed from the user

| What | For | Status |
|---|---|---|
| Card seated in the CPU x16 slot (`PCI_E1`), host powered back on | M0 | done 2026-10-04 |
| BIOS: primary display left on the integrated GPU (IGD) | M0 | done (iGPU is `boot_vga`) |
| Monitor connected to the 7570 | M0 (EDID, modeset trace) | done: DVI-I via DVI-to-HDMI adapter |
| Approval of the M0 host actions listed below | M0 | given 2026-10-04 (rebinding, module load/unload; trace in an x86 guest, no mmiotrace) |
| Host reboot after the kernel oops of 2026-10-04 | M1, M2 | done; `radeon` stays unloaded |
| Whether the slightly yellow white is the monitor or the signal (try `rdn_tool -d modeset`) | later | closed 2026-10-08: it was the old TV; the new monitor shows no tint |
| Which resolution counts as native | M2, M3 | decided: the EDID's preferred timing, 1366x768@59.79 |
| Mac OS X 10.4 PowerPC install DVD image | M1 | done: `media/tiger-install.iso` |
| Mac OS X 10.4.11 Combo Update (PPC) `.dmg`, if the DVD is older | M1 | not needed: Apple's Software Update still serves it |
| Xcode 2.5 Developer DVD `.dmg` | M1 | done: converted to `media/xcode25.img` |

Media goes in `media/` (git-ignored). Guest disk: 32 GB qcow2 (sparse).

## Milestone 0 — Know the card, capture ground truth

Success: PCI ID and memory type confirmed, VBIOS dumped and hashed, register
trace of init and of a modeset saved and documented.

Steps:

1. ~~**Card enumerates.**~~ Done: `0000:10:00.0`, `1002:675d` Turks PRO, own
   IOMMU group (10, with its audio function).
2. **What Linux says.** Done except EDID: family TURKS, 1024 MB **GDDR5**
   (not DDR3 as assumed), DisplayPort + DVI-I, firmware leaves the card
   un-POSTed. EDID read: preferred 1366x768@59.79, also 1920x1080@60.
3. **VBIOS dump.** Done; hashes in HARDWARE.md. `echo 1 > rom; cat rom > private/vbios.rom; echo 0 > rom`
   on the sysfs node. Read-only; nothing is written to the card. Record size,
   SHA-256 and MD5 in HARDWARE.md. Cross-check the memory type against the
   VBIOS tables.
4. **Register trace.** Done with the VFIO method in an x86 guest
   (`scripts/x86-trace-guest.sh`): cold init, fbcon modeset, two explicit
   modesets, blank and unblank. mmiotrace was not used.
5. **Document.** Done: `docs/REFERENCE-TRACE.md`.

Host actions that need approval before they happen:

| Action | Why | Risk |
|---|---|---|
| Power off, seat card, power on | Card not detected | Host downtime; needs physical access |
| Unbind `radeon` from the 7570 and bind `vfio-pci` at runtime | 4(a), and all later milestones | None to the host GPU; no persistent change |
| Load `radeon` with `modprobe`, unload with `rmmod` | Controlled init for tracing | `radeon` never binds to the Cezanne iGPU |
| Enable `mmiotrace` | 4(b) only | Host runs on one CPU while tracing |

Not planned: kernel parameters, initramfs changes, blacklists, anything on
`30:00.0`, any write to the card's flash.

## Milestone 1 — Tiger in QEMU with the card passed through

Success: the card shows in `ioreg` in Tiger as an `IOPCIDevice` with its BARs
assigned and accessible.

1. ~~Build QEMU (`ppc-softmmu`, `ppc-linux-user`) from source in `third_party/`.~~ Done 2026-10-04 (`scripts/build-qemu.sh`).
2. Install Tiger on `mac99` with emulated VGA; update to 10.4.11; install
   Xcode 2.5; enable Remote Login; install an ssh key. Snapshot.
3. Add `vfio-pci`. Inspect the OpenBIOS device tree and `ioreg`.
3a. Done 2026-10-04: OpenBIOS assigns BAR0 (256 MB) at `0x90000000` and BAR2
   at `0xa0000000` with no patch. It does not assign the I/O BAR or the ROM.
   Remaining: a probe kext that maps BAR2 and reads registers, to show the
   BARs are accessible. The kext takes the VBIOS from a file under `mac99`
   (see "Deferred to the real G5").
4. Expected trouble: OpenBIOS and the 64-bit 256 MB BAR0. Patch OpenBIOS or
   QEMU as needed; document in HARNESS.md. If unworkable, stop and present
   alternatives.

## Milestone 2 — Cold POST and modeset from Linux userspace

Success: from an un-POSTed card, the program shows a test pattern at the
monitor's native resolution (user confirms).

1. ~~Find and document a reliable way back to the un-POSTed state.~~ Done:
   `scripts/card-reset.sh` (secondary bus reset through the root port).
   - ~~Does VRAM work after `asic_init` without `TURKS_mc.bin`?~~ Yes, as far
     as the aperture test goes. No microcode loader is planned.
2. Hardware library behind a small OS layer (MMIO, config space, delay,
   memory, log). No Linux or IOKit dependencies inside it. OS layer done
   (`hw/rdn_os.h`).
3. VBIOS reader, AtomBIOS interpreter, `asic_init`, DDC/EDID, DCE5 modeset.
   - Done: interpreter (`hw/atom/`), bring-up (`hw/rdn_post.c`), verified
     against the reference trace without hardware and then on the card
     (`tools/rdn_tool.c`), with and without the I/O BAR.
   - Done: EDID over DDC (`hw/rdn_i2c.c`), identical to the EDID Linux read.
   - Next: the DCE5 modeset for the DVI-I digital
     output, validated by replay against trace phases `a3`/`a4` before it
     runs on the card. Then the test pattern and the user's visual check.
4. Big-endian test: build for PowerPC Linux, run under `qemu-ppc` against the
   VBIOS dump, compare the interpreter's register writes with the x86 run.
   Automated. Done for `asic_init` (`make test`); extend as code is added.
5. Compare against the milestone 0 trace before asking for a visual check.

## Milestone 3 — `IOFramebuffer` kext in Tiger on QEMU

Success: Tiger desktop on the 7570's monitor at native resolution, surviving a
resolution change from System Preferences (user confirms).

1. Kext skeleton matching on `IOPCIMatch`, built in the guest, loaded by hand.
2. Single fixed mode.
3. Modes from EDID, resolution and depth switching, cursor.

## Known gaps after phase 1

None of these blocks the phase 1 goal; each is a deliberate omission or an
unexplained observation.

Driver:

- **Topology is hard-coded** in `hw/rdn_modeset.c`: CRTC 0, pixel PLL 1,
  digital encoder 0, UNIPHY link A, hot-plug line 1, DDC line 0x93. It must
  come from the VBIOS object table before any other connector or card works.
  Since 2026-10-07 it is a table of this card's two outputs in
  `hw/rdn_modeset.c`, still not read from the VBIOS. The DisplayPort
  connector works on the G5 (`hw/rdn_dp.c`, 2026-10-07, one monitor); no
  hot-plug or wake handling, and a passive DisplayPort to DVI/HDMI adapter
  is not supported.
- **Modes** are only the EDID's detailed timings (two on the test monitor).
  No standard or established timings, no CEA modes, no scaling.
- **HDMI** signalling is used when the EDID asks for it, with the AVI
  infoframe (since 2026-10-05) but without audio. The boot argument
  `rdn_dvi=1` makes the kext use DVI signalling instead. The yellow-tinted
  white seen on the first TV was the TV (user, 2026-10-08); the new monitor
  shows none.
- ~~Line buffer and watermark setup~~ Ported 2026-10-06
  (`hw/rdn_watermark.c`, CRTC 0, one display); the user saw rows of
  stripes without it once the engine clock was raised, and none with it.
- **Power states**: with `--accel` the kext raises voltage and engine
  clock to the PowerPlay table's 650 MHz at start (`rdn_bootclocks=1`
  keeps the VBIOS boot clocks, 100 / 150 MHz). The memory clock goes to
  800 MHz only by hand (`rdnuc power performance 4`) and only when the
  memory controller's microcode was loaded at boot (`rdn_mc=1`); both
  work on the G5 (2026-10-06). No fan or thermal handling of our own
  (48 C after demo runs).
- **Cursor**: IOGraphics' software cursor by default; the card's hardware cursor (`hw/rdn_cursor.c`, `RDN_HWCURSOR=1`, `--hwcursor`) was confirmed by the user on 2026-10-05 and is opt-in.
- **8 and 16 bpp** were switched without error but not each inspected by
  eye for long.
- **No power management**: no DPMS, sleep or wake handling, no display
  hot-plug detection. The EDID is read once at start.
- **Unloading** the `IOFramebuffer` kext while the window server uses it has
  not been tried.
- **MC microcode**: loaded only with the boot argument `rdn_mc=1` (`hw/rdn_mc.c`, set on the G5 since 2026-10-06); it makes the 800 MHz memory clock work. Phase 1 and QEMU run without it, on the VBIOS's memory setup.
- **Framebuffer address range** is left where the BIOS put it
  (`0xF00000000`); Linux relocates it. Fine for scanout, to revisit for
  acceleration.

Harness:

- A framebuffer loaded with `kextload` after boot is only used once the
  window server restarts (`scripts/kext.sh activate`). Not an issue for the
  installed kext, which loads before it.
- QEMU needs `patches/qemu/0001-...`; OpenBIOS's `ranges` property and the
  bridge's address-select register still describe 256 MB.
- Unbinding the host's `radeon` from the card led to a host kernel oops;
  `radeon` is blacklisted on the host as a result.
- Apple's `installer` hangs after "Assembling receipt" on large packages in
  the guest.
- With two displays the scripted pointer (`guest-ctl.py`) lands off target.

## First run on the real G5 (not planned in detail yet)

`scripts/make-g5-package.sh --with-vbios` produces the package; the user
installs it with `sudo ./install.sh`. Alternatively the whole system:
`images/tiger-g5.qcow2` (2026-10-05) is a copy of the guest's disk with
that package installed (phase 1 kext, no acceleration keys) and unpacked
in `~/RadeonNI-g5`; it loads the kext at boot under QEMU. Write it to a
disk of 32 GB or more with `sudo qemu-img convert -p -O raw
images/tiger-g5.qcow2 /dev/sdX`. It has not booted on the G5. Things to look at first on the Mac:
`ioreg -p IODeviceTree -n pci1002,675d -w0` (did Open Firmware assign BAR0
and BAR2, and the ROM?), `sudo dmesg | grep RadeonNI`, and whether the
6600 LE stays the console.

## Deferred to the real G5

Things knowingly left untested under QEMU, to revisit once the driver runs
on the Power Mac G5.

- **VBIOS from the PCI expansion ROM. Done on the G5 (2026-10-06,
  journal).** Open Firmware assigns the ROM (128 KB at 0x80120000), the
  image read through it equals the file byte for byte, before and after
  POST, and the kext takes the VBIOS from there; the G5 runs with no VBIOS
  file installed. The file in the personality is the fallback, and the only
  source under `mac99`, where OpenBIOS does not assign the ROM BAR (user
  decision, 2026-10-04; OpenBIOS is not patched for this), so the ROM path
  never runs in the harness. Not seen: a ROM that does not answer, or a Mac
  whose firmware assigns no address (the code falls back to the file for
  both; neither has happened).

- **Open Firmware output (evaluated 2026-10-08, nothing built).** The card has
  no FCode, so Open Firmware shows nothing on it and there is no boot logo. The
  route and milestones M0 to M5 are in `docs/OPEN-FIRMWARE.md`; the order of work
  for the next session with the G5 is `docs/OPEN-FIRMWARE-PLAN.md`.

# Phase 2 — hardware acceleration

Planned with the user on 2026-10-04. Goal: Quartz Extreme, Core Image and
OpenGL 2.0 on the 7570 under Tiger, reached milestone by milestone. The
background and the evidence are in RESEARCH.md section 10.

## Approach

```
app / WindowServer -> OpenGL.framework -> GLEngine (gli*)
   -> RadeonNIGLDriver.bundle (gld* lifecycle; dispatch table -> Mesa)
        Mesa: GL state tracker + r600 gallium driver + "rdn" winsys
   -> IOUserClient -> RadeonNI.kext: IOAccelerator service
        hw/ accel core: CP ring, microcode, buffers, fences (shared C)
```

- Mesa's `r600` is the 3D driver. It is a complete GL implementation, so it
  cannot be an ordinary `gld*` driver under Apple's `GLEngine`; the bundle
  implements the lifecycle calls and gives the public GL dispatch table
  (`gliDispatch.h`) to Mesa. A0 showed the bundle can reach and change that
  table; `gldInitDispatch` does not hand it over, the engine's context does.
- The kernel half (command processor, memory, fences) is ported from the
  MIT-headed Linux `radeon` files into `hw/`, under the same rules as phase
  1, and shared by `rdn_tool` and the kext.
- First memory model: everything in VRAM (ring, command buffers, textures)
  through the 256 MB aperture. No GART, no bus mastering, no interrupts;
  fences are polled. GART and interrupts come in A7.

## Milestones

| # | Milestone | Success criterion | State |
|---|---|---|---|
| A0 | Prove the plug-in route, no hardware | A GL program in the guest runs through our `gld*` bundle and shows the effect of a dispatch entry we replaced | **Criterion met 2026-10-05** for an off-screen context: one entry (`glClear`) replaced. Replacing all 686 for the life of a context is still unproven (GLD-INTERFACE.md, "Not known yet") |
| A1 | Command processor from Linux userspace (x86, real card) | `rdn_tool` draws a triangle into the scanout buffer; user confirms | **Works by framebuffer readback 2026-10-05** (`rdn_tool accel`, `rdn_tool grab`); the user has not yet confirmed it on the monitor |
| A2 | Mesa on our winsys (x86 Linux, real card, no Linux DRM) | Mesa renders an animated test on the monitor; then the big-endian build under `qemu-ppc` does the same | **Works by readback 2026-10-05** on x86 and big-endian (`qemu-ppc`), pixel-identical; not yet confirmed on the monitor |
| A3 | Kext accelerator + Mesa in Tiger | A full-screen CGL program in the guest renders on the 7570 | **Done 2026-10-05**: kext engine and user client; Mesa built for Tiger; CGL contexts (off-screen, window and full-screen) render through Mesa on the card. Full-screen needs the surface path (default); Quake 3 full screen was seen by the user on the G5 |
| A4 | Windowed OpenGL | A windowed GL program on the desktop; a renderer query reports our renderer and GL 2.0 | **First form confirmed on the monitor by the user 2026-10-05** (tag `working-cpu-copy`): a GLUT program in a window renders through Mesa on the card (reports AMD TURKS, GL 3.2), by copying each frame into the window's buffer. Second form 2026-10-05, by readback, the default since the evening of 2026-10-05 (`/tmp/rdngld.nosurface` turns it off): the window is a window server surface, drawn by Mesa and shown by the card with no CPU copy; Apple's Chess is correct that way. Not seen by the user; shadows over a surface are wrong |
| A5 | Quartz Extreme | Quartz Debug reports it enabled; user confirms the effects | **Confirmed on the monitor by the user 2026-10-05** (QUARTZ-EXTREME.md): with all gates met the window server composites the 7570's display with OpenGL through Mesa on the card; desktop, windows, dragging and Exposé work. Open: it looks a bit slow. The software cursor flickered slightly; the hardware cursor (`RDN_HWCURSOR=1`, confirmed by the user) does not |
| A6 | Core Image | Hardware-rendered Core Image filters (Dashboard ripple) | **Works by readback and is the default (2026-10-08)**; the user chose that after seeing "ATI Radeon" as the renderer name, having disabled Core Image the night before (the window server then reported "Core Image: Not Supported"). Now: `GL_RENDERER` "ATI Radeon HD 7570" for every program, `GL_APPLE_client_storage` and `GL_APPLE_float_pixels` named, pbuffers on, and Core Image on the card in the window server unless `/Library/Application Support/RadeonNI/nocoreimage` exists. `ciprobe gl 0x21a00` equals `ciprobe soft` within 1 on nine filters, and all 100 built-in filters run on the card; 1.2 to 1.4 ms a render against 3 to 16 on the CPU. In the window server `wsfilter` and Dashboard's widget-drop ripple work; the ripple's flicker was fixed by poll and log but the user has not seen it again. Built: off-screen drawable fix, shared contexts, pbuffers (8-bit, 16-bit, float stores), hooks for CGL's and the window server's texture-of-a-surface call, a GPU copy of the screen's region. Not done: drawing into floating point pbuffers, long runs of ripples (memory given back through destroy hooks), Dashboard seen by the user (`docs/2D-ACCELERATION.md`, journal 2026-10-08) |
| A7 | Hardening | GART and interrupts, an accelerated 2D GA plug-in (the one from A5 uses the CPU), performance, piglit subset | started 2026-10-06: GART on the G5, on by default (`rdn_gart=0` leaves it off), programs' buffers in system memory, an overflow there when video memory is full. piglit subset run 2026-10-08 (`docs/PIGLIT.md`: 96 % of 4,962 GL 2.1 tests pass; the failures are correctness bugs; packed pixel types fixed the same day, 4,846 pass after). Not done: eviction (`docs/MEMORY-EVICTION.md`: three `large-tex` subtests still fail, not yet investigated), interrupts, an accelerated 2D plug-in, fixing the piglit clusters, a baseline on Apple's renderer |

A0 and A1 are independent and may run in parallel. If A0 fails, stop and
present alternatives (a `gli*`-level engine replacement, or a native `gld*`
driver without Mesa).

The real G5 is a parallel track that needs the user: first boot of the
phase 1 package, then each A-milestone repeated there. Its 6600 LE under
working Quartz Extreme is the only reference for A4 and A5, because the
guest has no accelerated device.

## Risks, in order

1. The dispatch takeover (A0); everything from A3 on depends on it.
2. Private structures in the `gld*` lifecycle calls (pixel format, renderer
   info, drawable).
3. The window server's requirements for Quartz Extreme cannot be observed
   under QEMU.
4. Mesa `r600` big-endian bugs. The newest release is used (26.2.4; user's
   decision, 2026-10-05: the big-endian fixes for this GPU are recent).
5. Tiger userland: no thread-local storage, missing libc functions, and a
   C++17 compiler and runtime, which current Mesa `r600` requires.
6. GDDR5 without the MC microcode under 3D load; load `TURKS_mc` if A1
   shows memory errors.
7. TCG speed: fine for correctness, useless for judging performance.

## Decisions (user, 2026-10-04)

- Apple's GL plug-in ABI may be learned by observation (symbol lists, a
  logging shim around Apple's software renderer, VMsvga2's MIT sources) and
  by reading disassembly to understand the interface. All code is written
  fresh; nothing decompiled is copied. Findings go in `docs/GLD-INTERFACE.md`.
- Microcode is handled like the VBIOS: git-ignored `firmware/`, injected at
  load time, never committed.
- Microcode, the command ring and (in A7) interrupts are in scope.
- Mesa and the GL bundle may be cross-compiled on the host; the kext stays
  guest-built.
- The GL bundle may be installed in the guest's `/System/Library/Extensions`
  if A0 shows the framework only loads it from there.
