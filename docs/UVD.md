# UVD (hardware video decode): status, architecture, how it was brought up

Read this before any work on video decode on the 7570. Status (2026-10-19): **the UVD block boots from our code
and Mesa's own UVD code decodes H.264 on it, bit-exactly, on the real G5** (x86 server, Tiger guest and G5 all start it).
Done: the kernel side (hw/rdn_uvd.c, the kext's user-client methods), the message layer (Mesa r600's `radeon_uvd.c` over our
winsys, see "The message layer" below) and a test that decodes I/P pictures, Baseline to High, against ffmpeg. Still to do:
the UVD clocks (the block runs on the bypass clock: 720p at 34 ms a picture), a VA-API-style entry to give parsed headers to
`pipe_h264_picture_desc`, the PowerVLC module and the QuickTime component, address checks in the kext. Older sections below are
the paper check and the failed first attempts, kept for the record; the journal (`docs/JOURNAL.md`, 2026-10-18 and 2026-10-19)
has the whole story. See also `docs/LEOPARD-VIDEO-DECODE.md`.

## What it takes (found by bisecting radeon's own register stream on a card we POSTed)

1. **Video memory below 4 GB**: `rdn_mc_relocate(card, 0)` (`hw/rdn_mc.c`; radeon's `evergreen_mc_stop`,
   `evergreen_mc_program`, `evergreen_mc_resume`) moves it from the BIOS's 0xF00000000 to 0. With it left where
   POST puts it the VCPU does not answer, with or without the address-extension registers. The kext does it
   when its personality has `RelocateVRAM` (`RDN_RELOC=1` in `scripts/kext.sh`); everything that needs the
   base reads `MC_VM_FB_LOCATION`. Before the mode set; the display controllers stay off afterwards.
2. The firmware in video memory (any 4 KB-aligned place below 256 MB), the first memory window only
   (`UVD_VCPU_CACHE_OFFSET0`/`SIZE0`) and `UVD_VCPU_CHIP_ID`.
3. `RLC_CNTL` = 1 (RLC enable) AFTER the window, and a few hundred ms before the VCPU is released. The RLC
   microcode upload is not needed.
4. `UVD_CGC_GATE` = 0, `UVD_VCPU_CNTL` = 0x200, `UVD_SOFT_RESET` = 0. No PLL, no LMI control (optional,
   `RDN_UVD_F_LMI`), no interrupt handler, no GART, no MC microcode, no GFX init.
5. Ring: submissions padded to 16 dwords with `UVD_NO_OP` (the controller fetches in 16-dword blocks); the
   fence of the evergreen hardware is `uvd_v2_2_fence_emit` (`UVD_CONTEXT_ID` = sequence number, the VCPU stores
   it at the DATA0/DATA1 address on command 0, command 2 traps).
3b. **The RLC microcode (`BTC_rlc.bin`, 768 big-endian words) must be uploaded before `RLC_CNTL` = 1**, as `evergreen_rlc_resume`
   does (`RLC_CNTL` = 0, the eight RLC_HB/MC/UCODE_CNTL registers to 0, 768 x `RLC_UCODE_ADDR`/`RLC_UCODE_DATA`, address 0, enable).
   The card keeps that microcode across a warm reboot and a bus reset and loses it only with power. On the server it was
   already there from earlier uploads, so "RLC_CNTL = 1 alone" seemed to be enough; after the card had been out of power
   (moved to the G5) the VCPU never started (firmware window untouched, `UVD_STATUS` 0). Kext: personality key `FW_RLC`
   (`RDN_UVD=1` in `scripts/kext.sh`), `rdn_uvd_set_rlc_ucode()`. Found 2026-10-19 on the G5 after ruling out Leopard vs Tiger,
   Open Firmware's POST, clocks, MC microcode and its order, the firmware blob, byte order, display traffic, Quartz Extreme.
In code: `RDN_UVD_F_MINIMAL` (and `RDN_UVD_F_LMI`) in `hw/rdn_uvd.c`; `rdnuvd init 3072` in the guest.
`UVD_STATUS` bit 1 alone is not proof (it also appears when the firmware does not run); the firmware modifies its own
window (40 bytes at image offset 320 become 7e706b85 611a2c7d 3062109e 30260b85 00031e40, as under radeon) and the ring test
is the real check.

## The message layer: Mesa's r600 UVD code over our winsys (2026-10-19)

Nothing was copied from Mesa and no module was written: r600 already compiles `radeon_uvd.c`, `r600_uvd.c`, `radeon_video.c`, and
the screen offers `create_video_codec`/`create_video_buffer` when the winsys says `info.ip[AMD_IP_UVD].num_queues` is not 0.
What the winsys (`mesa/winsys/rdn_winsys.c`) does for it:

1. A UVD command stream (`rdn_cs_create(AMD_IP_UVD)`; the decoder is started by the first one: `rdn_device::uvd_init`). Its
   words are `radeon_uvd.c`'s legacy ones: `GPCOM_VCPU_DATA0` = offset in the buffer, `DATA1` = 4 x the buffer's index in the
   stream's list. `rdn_uvd_relocate()` turns each pair into the 40-bit address, as the radeon kernel's `radeon_uvd_cs_reloc`
   does. The IB goes to visible video memory, padded to 16 words with type-2 nops (`radeon_drm_cs.c`), then `uvd_submit`.
2. Fences of their own (`rdn_fence.uvd`, `rdn_busy.uvd`). Rings are not synchronised by the hardware here: a UVD stream waits on
   the CPU for the 3D engine's work on its buffers; the 3D engine, CPU maps and destroys wait for the buffer's `last_uvd`.
3. Placement: the decoder reads its messages from the 256 MB its firmware is in, so r600's message/bitstream buffers (GTT domain,
   but shareable: no `RADEON_FLAG_NO_INTERPROCESS_SHARING`) go to visible video memory, not to the GART. Decoded pictures are
   linear (`R600_UVD_ENABLE_TILING` is 0) NV12 in visible video memory.
4. Byte order (Mesa patch `0014-uvd-message-little-endian.patch`): the message and the feedback buffer's first word are
   little-endian words (swap of the 32-bit members and the one 16-bit pair, 8-bit members stay); the bitstream and pictures are
   byte streams. Only H.264 decoders are created on a big-endian host. Reading a picture with a GPU copy (r600's transfer
   map does) swaps the bytes of every 16-bit texel the GPU did not write, so U and V of NV12 come out turned around: read the
   buffer itself, or fix the swizzle where the planes are sampled.

Test: `mesa/tests/rdn_uvdtest.c` (build target `rdn_uvdtest` of `scripts/build-mesa.sh darwin`), with vectors from
`scripts/uvd-testvec.sh` (ffmpeg/x264 on the host; the reference is ffmpeg's decode). On the G5 (Leopard): bit-exact luma and chroma
for Baseline I (500 repeats), Baseline IPPP with 3 references, Main CABAC with 2 references, High CABAC 720p with 4 references.
720p takes 34 ms a picture: the UPLL is in reset and bypass after the minimal start (`CG_UPLL_FUNC_CNTL` = 0x305), so
`RDN_UVD_F_PLL` brings the clocks up (Linux's 533/400 MHz) at the first command stream and keeps them while a decoding client is open.
Kext hardening still open: the commands are not checked (addresses, 256 MB segment), as the 3D path's are not.
With the PLL option (`RDN_UVD_F_PLL`, kext default; clocks up at the first command stream, down when the last client that
decoded closes) 720p High CABAC takes 9.2 ms a picture on the G5 (34 ms before).

**Getting pictures out**: the CPU reads video memory through the aperture at 1.2 MB/s (8-bit loads), 4.6 (32-bit) or 9.1 MB/s (64-bit)
(`rdnuvd bw`; writes run at 600-750 MB/s), so a 720p picture would take 150 ms and 1080p 340 ms to read. A consumer must either keep the
picture on the GPU (convert and show it with the 3D engine; zero copies) or have the GPU copy it into system memory (GART, `gart_alloc`) and read that.
Mind the byte order of such a copy: the 3D engine's copy of an R8G8 plane swaps U and V on this big-endian host (see 4 above).

## Why

PowerVLC 2.1.0 on the G5 decodes H.264 on the CPU (ffmpeg). 1080p H.264 drops nearly every frame
(journal 2026-10-17); 704x400 MPEG-4 plays. The GPU only converts colour and scales.

## What the card has

- Turks (HD 7570) has a UVD block (UVD 3.x on Northern Islands; MPEG-2, H.264, VC-1, MPEG-4
  part 2). [I] from Linux treating Turks like Cypress/Barts in `radeon_uvd.c`.
- Firmware: Linux loads `radeon/SUMO_uvd.bin` for `CHIP_TURKS` [V `radeon_uvd.c`, the `SUMO`
  case lists SUMO, PALM, CAYMAN, BARTS, TURKS, CAICOS]. On this host:
  `/lib/firmware/radeon/SUMO_uvd.bin.zst`, 130075 bytes compressed. Not in `firmware/` yet; it would
  go there like the other microcode (git-ignored, injected, shipped unmodified with its licence).

## What exists in Mesa 26.2.4 (`third_party/mesa-26.2.4`)

- `src/gallium/drivers/r600/radeon_uvd.c` and `r600_uvd.c` implement the decoder: they build the
  UVD message, feedback, bitstream, DPB and context buffers and submit them through the winsys.
  [V] `r600_pipe.c`: `create_video_codec` is set only if `info.ip[AMD_IP_UVD].num_queues != 0`.
- The decoder needs from the winsys: `cs_create(..., AMD_IP_UVD, ...)`, `cs_add_buffer`,
  `buffer_get_virtual_address`, `buffer_map`, `cs_flush`, `fence_wait`
  [V `radeon_uvd.c`]. Our winsys (`mesa/target/rdn_target.c`) has the graphics ring only.
- Mesa converts picture parameters (`pipe_h264_picture_desc`) to UVD messages itself; a program
  would feed it slices and parameters.

## What Linux `radeon` does to start UVD (the part we would port into `hw/`)

- `radeon_uvd_init`: firmware into a VCPU buffer. `uvd_v2_2_resume` (and `uvd_v1_0_resume`):
  program the memory-controller window (`UVD_LMI_ADDR_EXT`, `UVD_LMI_EXT40_ADDR`, firmware offset
  and size). `uvd_v1_0_start`: reset, `UVD_LMI_CTRL`, swap control, boot the VCPU, set up the ring
  (`UVD_RBC_*`). `uvd_v1_0_init`: ring test.
- Clocks: `evergreen_set_uvd_clocks` programs the UPLL (VCLK/DCLK dividers, 53300 and 40000 in
  10 kHz units while decoding, 10000 idle) [V `uvd_v1_0.c`, `evergreen.c`].
- Big endian: `uvd_v1_0.c` has `#ifdef __BIG_ENDIAN` setting `UVD_LMI_SWAP_CNTL` and
  `UVD_MP_SWAP_CNTL` [V]. So the engine itself can swap; messages from Mesa are little-endian
  structures that the big-endian host writes, as for the graphics ring (`mesa/patches/0006`,
  `0007`).
- Memory rule: message and feedback buffers must lie inside one 256 MB segment and relocations may
  not cross a 256 MB boundary [V `radeon_uvd.c`, lines ~600-615]. Our heap is 222 MB inside the
  256 MB aperture [V `rdnuc info`], so it fits if decode buffers come from there.

## What would have to be built

1. `hw/`: UVD bring-up (firmware, MC window, clocks via AtomBIOS PLL tables, start, ring test),
   behind `hw/rdn_os.h` like the rest.
2. Kext: a UVD ring, a user-client method to submit a UVD command buffer by GPU address, a fence
   for it, power/clock management so the UVD block does not stay clocked.
3. `mesa/target`: a UVD queue in the winsys (`info.ip[AMD_IP_UVD]`), so `r600_uvd` loads.
4. Mesa for Darwin: build the r600 video code (`radeon_uvd.c`, `vl`) into the bundle or a
   separate library; check whether `src/gallium/auxiliary/vl` builds on big-endian Tiger/Leopard
   (`bool` size rule in `CLAUDE.md`).
5. VLC side: ffmpeg's H.264 hwaccel hooks (VDPAU-style) or VLC's `vlc_va_t` interface (what
   VideoToolbox and VA-API modules implement), as a new PowerVLC plug-in that converts to
   `pipe_h264_picture_desc`, decodes into a video buffer, and hands the frame to the GL output
   as a texture (same card, no copy).

## Risks and unknowns

- The block may not start on this board without the right UPLL setup; nothing has read a UVD
  register on this card. Reads of UVD registers while the block is unclocked are not known to be
  safe; do not try on the host without the user's yes and `scripts/card-quiet.sh apply`
  (`CLAUDE.md`, host safety rules).
- Whether decode targets must be in VRAM or may be in GART memory [unknown].
- Performance: UVD3 decodes 1080p H.264 up to level 4.1 on Linux (VA-API users) [I]. UVD does
  the entropy decoding too, so the CPU would only parse headers and slice boundaries [I]; the
  PCIe x4 link carries just the bitstream.
- Effort: large (weeks), mostly steps 2 and 5.

## Suggested order

1. Extract `SUMO_uvd.bin` into `firmware/` and read its header (version, size) offline.
2. On the Linux tool path with the card quieted (user's yes first): clock the UPLL and boot the
   UVD VCPU with the firmware, read `UVD_STATUS`; stop there if it does not come up.
3. Ring test, then one H.264 I-frame message from Mesa's own `radeon_uvd.c` through a test
   program, compared with a CPU decode.
4. Kext and winsys work, then the VLC module.

## Step 1 result (2026-10-17): the firmware file

- `firmware/SUMO_uvd.bin` (git-ignored, like the other microcode), extracted with
  `zstd -dc /lib/firmware/radeon/SUMO_uvd.bin.zst`: 205080 bytes (a multiple of 4), md5
  `51d9e0e2247c313c5bfc8fa7bb5b213d`. The redistribution licence is the same `LICENSE.radeon` as for
  the other microcode in `firmware/`.
- It is the **legacy raw image**: no `common_firmware_header` (Linux then sets `fw_header_present`
  false and cannot print a version), no readable strings, 7.1 bits of entropy per byte, 205080
  bytes with the last non-zero byte the very last one. So version and family cannot be read
  offline; Linux reads the firmware version back from the running block. Loading it needs no
  parsing: the whole file is copied to the start of the VCPU buffer
  (`radeon_uvd.c`, `memcpy_toio(... uvd_fw->data, uvd_fw->size)`), followed by stack, heap and
  the per-session areas (`RADEON_UVD_STACK_SIZE`, `RADEON_UVD_HEAP_SIZE`,
  `RADEON_UVD_SESSION_SIZE` times the handle count) in one VRAM object.
- Nothing was run on the card.

## Architecture proposal: one kext, two front ends

```
 PowerVLC (VLC 3, ffmpeg)         QuickTime-based programs (QT Player, iTunes, Safari plug-in)
   vlc_va_t module "radeonni"       image decompressor component "RadeonNI AVC" ('imdc', 'avc1')
        |  parsed picture params         |  raw access units (avcC, length-prefixed NALs)
        v                                v
 +------------------------------------------------------------------+
 | libradeonni-video.dylib  ("rdnvd", user space, MIT)               |
 |   session API: open(codec, profile, w, h) / decode(picture) /     |
 |   frame(surface, NV12 pointer or GL texture) / close              |
 |   H.264 header parser (SPS, PPS, slice header, POC, ref lists)    |
 |   UVD message builder = Mesa r600 radeon_uvd.c/r600_uvd.c (MIT)   |
 |   buffers: our own allocator in the aperture (RDN_UC_ALLOC)       |
 +--------------------------+---------------------------------------+
                            | IOConnect (existing user client)
                            v
 +------------------------------------------------------------------+
 | RadeonNI.kext                                                     |
 |   hw/rdn_uvd.c   firmware, MC window, UPLL clocks, VCPU start,   |
 |                  UVD ring, ring test, idle power-down             |
 |   RDN_UC_UVD_SUBMIT(ib address, dwords) with a check that every   |
 |       address in the message and feedback buffers is inside the    |
 |       caller's own allocations and the first 256 MB segment;      |
 |   RDN_UC_UVD_FENCE_WAIT, RDN_UC_UVD_CAPS (codec levels, max       |
 |       sessions), one lock for the ring, sessions per client       |
 +------------------------------------------------------------------+
```

- **One decode core, two doors.** Everything that knows the engine lives in the kext and
  `rdnvd`; the two front ends only translate. PowerVLC already has ffmpeg's parsed picture state
  (ffmpeg's hwaccel interface is VDPAU-shaped: `VdpPictureInfoH264` plus slice buffers), so its
  module maps that to the `rdnvd` picture structure. QuickTime hands raw samples, so `rdnvd`
  also takes an access unit and parses headers itself (needed: UVD wants POC, reference lists
  and the like). The parser must be written fresh or taken from MIT code (Mesa's
  `src/gallium/frontends/omx` parses H.264 headers); not from ffmpeg (LGPL), the project is MIT.
- **Why a user-space library and not decoding in the kext.** Parsing and message building are
  large, change often and can crash; in the kext only the engine, the ring, the address check and
  the power state belong. The existing accelerator user client (`RDN_UC_*`, `hw/rdn_user.h`)
  already carries allocations and fences, so the UVD calls are three more methods.
- **Frames out.** Milestone A: the decoded NV12 surface is read back to the CPU and given to the
  program (PowerVLC's existing output converts it on the GPU; QuickTime composes on the CPU
  anyway). Milestone B for PowerVLC only: `rdnvd` and the GL bundle share one Mesa, so the
  decoded surface can be sampled as a texture without a copy (like VLC's VDPAU/VA-API GL interop
  converters).
- **QuickTime specifics.** A component registered for `avc1` with a higher priority than
  `QuickTimeH264`; `ImageCodecPreflight` answers "not supported" when the engine is absent or the
  stream is outside UVD's limits (profile, level, size, interlace), so QuickTime falls back to
  Apple's software decoder. It is a documented API (the way Perian added codecs). The component
  is a PowerPC binary loaded by the program; the kext is the only privileged part.
- **Safety and arbitration.** One ring, so submissions are serialised under one kext lock; each
  user client's sessions are closed (UVD handles destroyed) when it dies, like the surface
  clients. The kext, not the library, enforces the 256 MB rule and that a message only names
  buffers the caller owns, as Linux's `radeon_uvd_cs_reloc` does.
- **Big endian.** The library writes UVD messages as little-endian words (Linux uses
  `cpu_to_le32`); the LMI/MP swap registers (`uvd_v1_0.c`, `#ifdef __BIG_ENDIAN`) are set so that
  the firmware sees the layout it expects. Which swap setting keeps the bitstream bytes in order
  is an open question for the first decode.

### Milestones (each needs the user's yes before it runs on a card)

1. Linux tool path, card quieted: UPLL clocks, firmware, VCPU start, ring test (`UVD_STATUS`).
2. Same path: one H.264 I-frame through Mesa's `radeon_uvd.c` message builder, compared with a
   CPU decode. Decide the swap settings here.
3. `hw/rdn_uvd.c` in the kext, three user-client methods, a test tool on Tiger/Leopard.
4. `rdnvd` with a command-line decoder: time 1080p H.264 against ffmpeg on the G5.
5. PowerVLC module (readback), then zero-copy to GL.
6. QuickTime component.

## Corrections after review (2026-10-17)

- **Where bring-up runs.** The 7570 is in the G5 and the user plays games on it; the Linux tool
  path and the QEMU guest both need the card in the server, which nobody asked for. Milestones 1
  and 2 therefore go through the kext on the G5: the user client has `RDN_UC_REG_READ` but no
  register write, so a first UVD bring-up means `hw/rdn_uvd.c` in the kext (built in the guest,
  installed by the package, restart) driven by a small test tool. A hang then costs the G5 a
  power cycle (journal: two after an Open Firmware hang), so it needs the user at the machine.
- **The H.264 parser.** Mesa 26.2.4 has no H.264 header parser. Its OMX frontend, which had one,
  is gone from the tree; the VA frontend (`frontends/va/picture_h264.c`) copies parameters an
  application already parsed into `pipe_h264_picture_desc`. What Mesa does have and can be
  linked: the bit reader `src/util/vl_rbsp.h` (+ `vl_vlc.h`), `radeon_uvd.c` (message builder,
  takes `pipe_h264_picture_desc`), and the whole VA frontend, which is the translation from
  libva structures. So the parser has to come from the caller: ffmpeg's own H.264 decoder can
  run in hwaccel mode (VDPAU-style: it parses, then hands `VdpPictureInfoH264` and the slice
  bytes to a callback), and that works the same for PowerVLC and for a QuickTime component that
  links libavcodec dynamically (LGPL: linked and shipped unmodified, not copied).

## Kext path, state of 2026-10-17 (nothing run on the card yet)

- `hw/rdn_uvd.c` / `rdn_uvd.h` / `rdn_uvd_reg.h`: clocks (UPLL), firmware copy, memory window, VCPU
  start, ring, indirect buffer submit with the VCPU's fence write, stop. Ported from Linux
  `uvd_v1_0.c`, `uvd_v2_2.c`, `radeon_uvd.c`, `evergreen.c`. `tests/uvd_model.c` runs it against a
  model of the block natively and under `qemu-ppc` (identical output) and reads the PLL reference
  clock (27 MHz) from the real VBIOS. The model only checks order and byte order.
- The big-endian question is answered by Linux's own code: on a big-endian host
  `UVD_LMI_SWAP_CNTL` is 0xa (swap "8 in 32" for the ring and indirect buffers only) and
  `UVD_MP_SWAP_CNTL` 0. Commands are therefore in host order, messages and bitstreams are not
  swapped (little-endian words, plain bytes).
- Kext: `RDN_UC_UVD_INIT`, `RDN_UC_UVD_SUBMIT`, `RDN_UC_UVD_FENCE_WAIT`, `RDN_UC_UVD_INFO`
  (`hw/rdn_user.h`). Inert unless the personality has `FW_UVD` (the firmware as in the file) and a
  program calls init: nothing touches UVD at load. A failed start is not retried until the kext
  is loaded again. The submit method does not yet check the addresses inside a UVD message; it only
  accepts an indirect buffer inside the client heap.
- `tools/guest/rdnuvd.c`: `info`, `init`, `test` (runs one indirect buffer that writes
  `UVD_CONTEXT_ID` and waits for its fence), `regs`. Builds on the G5 itself.
- Built: Tiger kext in the Tiger guest, Leopard kext in the Leopard guest
  (`LEOPARD_NATIVE=1 scripts/leopard.sh build`, `check`: 0 unknown symbols).
- Install on the G5 without repackaging: stage in `~/uvd-test/` (new `RadeonNI`, `SUMO_uvd.bin`,
  `install-uvd.sh`, `rdnuvd`); `sudo ./install-uvd.sh` keeps the old binary and plist as
  `RadeonNI.before-uvd-20261017` and `Info.plist.before-uvd-20261017` in the kext's `Contents`,
  adds `FW_UVD` (the perl edit was tried on a copy: valid plist), touches `Extensions`; then a
  restart. The first `rdnuvd init` is the first time this card's UVD block is clocked and booted: if
  it hangs, the G5 needs a power cycle.

## Result of the first bring-up attempts (2026-10-17 evening)

The VCPU does not answer on the G5 (`UVD_STATUS` 0) with a register sequence that now matches the
Linux reference trace of this card write for write (see the journal). The cache-register offset
mistake (R6xx instead of Evergreen) was found and fixed; it was not the whole story. What is
known: the PLL locks, the firmware is in video memory, every register reads back, no memory
controller fault is recorded. Open: the memory base (Linux moves it to 0, we keep the BIOS's
0xF00000000), and other state Linux's full init sets up. The next step that would settle it is
Linux's own behaviour on this G5 (registers and memory base while UVD runs), i.e. the ArchPOWER
route of the earlier proposal; or, inside the kext, trying the block with its memory reached
through the GART. `rdnuvd init <flags>` and the failure dump in the kernel log are the tools.
