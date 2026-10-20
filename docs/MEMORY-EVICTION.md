# Video memory, the GART and eviction (A7)

Written 2026-10-08 so that another session can pick this up cold. Status:
**nothing is implemented; one bug in the GART chunks was fixed on the way;
the three piglit subtests that still fail have not been investigated.**

## The question

piglit's `large-tex` (`tests/texturing/large-tex.c`) makes R8 textures of
16384x16384, 256 MB each, and runs 14 subtests that each hold two or three
large objects (textures, a 256 MB PBO). On the G5 11 of the 14 pass. The three
that fail (`get_tex_image_pbo`, `copy_pixels`, `image_load_store`) end in
`GL_OUT_OF_MEMORY` from our winsys ("rdn: out of video memory ... bytes
asked, ... in use").

I first said that nothing could help, because all of it is live. That was
wrong. On other systems it works, see the next section, and it is not known
why the three fail here.

## How other systems run more than the card's memory

- **Linux `radeon`/`amdgpu`.** TTM manages video memory (VRAM) and system
  memory behind the GART (GTT) as one pool. A buffer object lives in one of
  them and the kernel moves it ("evicts" it from VRAM to GTT, and back) when
  room is needed, as long as no command stream in flight uses it. Only the
  buffers one command stream names have to fit together. Mesa's winsys also
  keeps a budget of VRAM and GTT in use and flushes when it is passed. The GART
  size is a module parameter (512 MB or more by default).
- **Windows (WDDM)** pages GPU memory to system RAM by itself.
- **Mac OS X (Tiger's own OpenGL)** keeps textures in system memory and uses
  video memory as a cache.
- An out-of-memory error appears only when what one command needs does not
  fit, or system memory is gone.

## What we have

All in `mesa/winsys/rdn_winsys.c` (Mesa's side), `mesa/target/rdn_device_darwin.c`
(Tiger's device layer) and `kext/RadeonNI/RadeonNIAccel.cpp` (the kernel half).

- **Three places for a buffer** (`enum rdn_place`):
  - `RDN_VISIBLE`: the 256 MB aperture (BAR0), CPU-mappable.
  - `RDN_HIDDEN`: the 768 MB of video memory beyond it, never mapped by the CPU
    (tiled textures, render targets; `RADEON_FLAG_NO_CPU_ACCESS`).
  - `RDN_GART`: the program's own memory, wired and entered in a GART table
    (`hw/rdn_gart.c`, 1 GB of GPU address space at `0x40000000`). Used for
    GTT-domain buffers (uploads, staging) and as an overflow.
  - `RDN_USER`: the program's memory bound without a copy (`buffer_from_ptr`).
- **Placement** (`rdn_buffer_create`): where the buffer wants to go, else the
  aperture, else (after `rdn_cache_trim`) the aperture again, else the GART as
  an overflow ("Nothing here can move a buffer out of it to make room").
  **A buffer never moves after it is made.** There is no eviction.
- **Limit on the GART:** `GART_MOST_BYTES` (512 MB) of the program's memory
  bound at a time. Above about 860 MB the kext's bind call (`gartBind`, wiring
  with `IOMemoryDescriptor::prepare`) never returned and the process could not
  be killed (Call of Duty 2; journal 2026-10-07). Chunks of 16 MB
  (`GART_CHUNK_BYTES`) are kept bound for small buffers; a buffer over 16 MB
  gets a chunk of its own, unbound when it is freed.
- **A cache** (`rdn_cache_*`) keeps released memory for reuse and gives it back
  in `rdn_cache_trim(ws, true)`.
- **Diagnostics:** `RDN_STATS=1` in a program's environment prints the memory
  by place and size class when an allocation fails, and (since 2026-10-08)
  why a GART chunk was refused, with the table of chunks.

## The bug found on the way (fixed)

The third 256 MB buffer was refused "over the limit" with only 1 MB in use. A
chunk made for one large buffer was exactly the buffer's size; the chunk's
start in the GART is only page aligned and `rdn_mem_alloc` moves a buffer for
its alignment, so it did not fit; the call failed and the new chunk stayed
bound and empty, counted against the 512 MB. Fixed in
`gart_new_chunk`/`dev_gart_alloc` (room for the alignment; a chunk whose buffer
could not be made is given back; idle 16 MB chunks are released when a large
one is wanted). Journal: "2026-10-08: piglit `large-tex` (GART chunks)".

## What to do first

1. **Find out what each of the three subtests allocates and where it fails.**
   Build the bundle (`scripts/build-mesa.sh darwin
   src/gallium/targets/rdn/RadeonNIGLDriver.dylib`), install it on the G5
   (`scripts/mac.sh g5 ...`, see the `test-macs` skill, and keep the old one
   under a name) and run
   `cd ~/piglit && PIGLIT_SOURCE_DIR=$PWD DYLD_LIBRARY_PATH=$PWD/lib
   RDN_PIGLIT_MESA=1 RDN_STATS=1 bin/large-tex -auto` (stage piglit first:
   `docs/PIGLIT.md`, "How to run it"). The memory dump says how much is in each
   place when it runs out.
2. **Rule out leftovers.** Each subtest deletes its objects. Check whether the
   deleted textures are still held when the next subtest starts: Mesa defers
   some deletes, and the winsys cache keeps released memory until
   `rdn_cache_trim`. If so, trim earlier (before an overflow, and when the
   amount held is large) and some or all of the three may pass without
   eviction.
3. **Count what one command needs.** `get_tex_image_pbo` is one 256 MB texture
   and one 256 MB PBO: that should fit (768 MB beyond the aperture + the GART
   chunk of 256 MB). If it does not, something else is holding memory.

## If eviction is needed

Design notes, not a decision (the user decides):

- A buffer needs a **place that can change**: today `bo->offset` is used by
  Mesa as the GPU address when it emits commands (relocations in `rdn_cs`), so a
  move must happen before a command stream is built, or the winsys must patch
  addresses. Look at how `rdn_cs_*` in `rdn_winsys.c` turn a buffer into an
  address; this is the central question.
- Moving a buffer from video memory to the GART (or back) is a GPU copy
  (`hw/rdn_blit.c`, the CP's copy) plus a fence; the kext has no move call today.
- Which buffers may move: not those referenced by a command stream still in
  flight (`bo->last_use` fence), not those the CPU has mapped, not scanout.
- Simplest useful form: **demote on overflow.** When video memory is full, move
  the least recently used idle buffer to the GART instead of putting the new one
  there; promote nothing. That needs the move above and an LRU list.
- The GART limit (512 MB, from the kext hang) caps how much this can give. A
  fix for that hang (what the kext does when wiring many pages) would raise it;
  the cause is not known (a guess is the G5's DART).
- Mesa's own budget hooks: `radeon_winsys::query_value(RADEON_VRAM_USAGE)` is
  answered from `ws->allocated_bytes` (a rough number); r600 flushes when
  `vram_usage` passes a threshold of `vram_size`.

## Where this stands in the plan

`docs/PLAN.md`, A7 ("eviction"). Do not start before a program runs out of
memory in practice; so far none has (Call of Duty 2's overflow was the vertex
range, fixed another way). The piglit subtests are the only reason to look.

## What it would take, as of 2026-10-20 (a design answer; nothing built)

Asked after the decoder's buffers were moved out of the aperture (split mode, `docs/UVD.md`), which is eviction's cousin done by hand for one client.

**The obstacle is that addresses are absolute.** The winsys reports `r600_has_virtual_memory = true`, so Mesa's r600 writes `gpu_address` (cached in every `r600_resource` at creation) straight into command
packets, texture and buffer descriptors and state objects, and the kext allocator never moves anything. A buffer that moved would leave stale addresses in queued and in-flight command buffers and in the descriptors of
bound textures. Linux solves it in one of two ways, and **neither is what the card's family uses by default in Linux**:
- *Relocation + command checker* (what `radeon` does on Barts, Turks and Caicos: `btc_asic` has no `.vm` entry, `r600_has_virtual_memory` is false): Mesa emits `PKT3_NOP` relocation indices after every packet that
  carries an address, the kernel's checker (`evergreen_cs.c`, MIT, in `third_party/linux`) parses each indirect buffer, patches the real addresses in and TTM has placed (and may have moved) the buffers before the
  submission. Needs: the checker ported into the kext (about 4000 lines for Evergreen; it also validates registers, which this kext's recorder only warns about today), Mesa told to use relocations (the code is there:
  the `has_vm` branches in `evergreen_state.c`), the winsys passing buffer lists with the indirect buffer.
- *GPU virtual memory* (Linux enables it only on Cayman, Trinity and the Southern Islands parts: `radeon_vm.c`, `ni.c`): every buffer gets a fixed virtual address and the page tables say where its pages are; a move is
  a copy plus a page-table write. The kext already runs context 0 (the GART). Whether Turks' hardware has working contexts 1 to 7 is not known (the Linux driver never turns them on for it); it would need testing on the card.

**What both need besides, in the order I would do it:**
1. *The wiring limit first.* Eviction to system memory needs somewhere to evict to, and `gartBind` hangs above about 860 MB wired (the 512 MB cap): find out why (the DART?).
2. *A buffer record in the kext* instead of a list of offsets per client: size, placement (aperture, hidden, GART), last fence, mapped and pinned flags, an LRU list, owner. The winsys's `last_use`/`last_write` fences exist but only in the program; they must reach the kext.
3. *The move primitive*: now there (`rdn_cp_dma`, 2026-10-20: GART to video memory and back, ordered on the ring, with a fence); a move is a copy, a fence wait and the new address, never of a buffer a stream in flight uses.
4. *Validation at submit*: every buffer a stream names resident where the engine can read it, evicting least recently used idle ones for room, failing (or splitting the stream: Mesa's `used_vram_kb` hook does that already) when the working set is larger than video memory.
5. *The CPU side.* The whole aperture is mapped in every program, so a CPU pointer is aperture + offset and a move leaves it pointing at someone else's data. A map must pin (the winsys's `buffer_unmap` is a no-op today, and glthread keeps uploads mapped), or CPU-written data must have system memory as its home (stable pointers, the GPU reads it through the GART or from a cached copy in video memory: what Tiger's own OpenGL does).
6. *Write-back*: a buffer the GPU wrote (render targets) needs a copy to system memory before it can go; the stream's `RADEON_USAGE_WRITE` flags say which.

**What can never move:** the screen's surfaces and anything the window server was given an offset for (`RDN_UC_SURFACE_BUFFER`, Quartz Extreme), the kext's own memory (rings, GART table, firmware windows), and the decoder's buffers
(it addresses physical video memory in one 256 MB segment; they stay pinned while a session lives, which split mode already does).

**Whether to do it:** nothing has run out of memory in practice since the decoder's buffers left the aperture; the only users are the three piglit `large-tex` subtests. The cheap middle path that was taken for the
decoder (put the GPU-only bulk in the hidden 768 MB, keep what the CPU writes in the aperture) is the same idea without a mover, and could be extended to textures and render targets (the winsys already sends
`NO_CPU_ACCESS` buffers to the hidden memory). Eviction proper is a large piece of kernel work for a problem that does not yet exist.
