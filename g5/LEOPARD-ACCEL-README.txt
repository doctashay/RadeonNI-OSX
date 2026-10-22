RadeonNI for Mac OS X 10.5 (Leopard, PowerPC), with OpenGL acceleration
=======================================================================

A driver for the AMD Radeon HD 7570 / HD 6670 family (Turks: PCI IDs
1002:675d, 6758, 6759, 675b, 675f). It has been tested on a Power Mac G5
Late 2005 (PowerMac11,2) with a 1002:675d card under Mac OS X 10.5.8
(9L31a) and 10.5.9 (9L31b). It remains experimental.

On 10.5.8, the card reports 1024 MB and offscreen color and triangle draws
pass pixel readback. The driver reports OpenGL 2.1 to applications because
Leopard's OpenGL framework lacks the core entry points added in later versions;
Mesa 26.2.4 remains the renderer underneath. The screen-surface exception
allows Leopard's OpenGL compositor to start without the QE login helper.
GPU submissions and improved cursor dragging have been observed, but blank
menus and flickering dialogs remain. System Profiler's acceleration labels
do not establish correct QE or hardware Core Image. Core Image is disabled
in WindowServer by the installed nocoreimage marker on the 10.5.8 test host.
The packaged binaries may predate these source changes; this is not a release
validation. The optional QE-AUTOSTART helper remains off by default.
The driver omits GL_ARB_framebuffer_object so applications use Leopard's
exported GL_EXT_framebuffer_object entry points.
Window movement and some applications are still slow or fail to draw. The
current stable display mode is 1280x800; higher modes have shown scanlines.

Compositor review, 2026-10-18 (10.5.8)
------------------------------------
Confirmed coordinate defects: viewport/scissor queries exposed translated
screen coordinates, so save/restore applied the offset twice. Offscreen FBOs
also received the screen offset. The guest viewporttest reproduces these;
it also checks that FBO binding preserves the logical viewport.

Unresolved contracts found in the source:
- RadeonNISurface setShapeBacking and setShapeBackingAndLength ignore the
  supplied backing memory while returning success. flush hides its error.
- RadeonNIGA cannot allocate surfaces needed by IOFBBlitSurfaceCopy. The
  running system reports Copy screen to surface failures.
- rdn_watch_end can choose a surface by geometry/proximity instead of its
  identity. That cannot establish correct content ownership for overlapping
  windows. Origin and watch state also remain process-global.
- WindowServer drawable attachment treats non-pbuffer records as screen
  surfaces; drawable-type handling needs explicit validation.
- Fence waits have no GPU recovery path. Replacing waits with successful
  timeouts would allow reuse of GPU-owned memory and is not a safe fix.

Bounded upload captures include a complete 208x284 menu image and opaque
white 200x277 images. This proves the white pixels exist before GPU upload,
but does not yet establish why WindowServer substitutes those images or
whether they represent every failing menu frame. Menu correctness and the
reported Software Update flicker are still unresolved.

Portal stability investigation, 2026-10-18 (10.5.8)
--------------------------------------------------
Portal still hung after the winsys buffer-fence publication correction.
R600_TRACE reproduced a GPU stall with the CP trace counter at marker 83;
the adjacent captured indices and vertex positions did not show an obvious
out-of-range access. The cause of the gameplay hang remains unresolved.

Disabling Mesa glthread allowed the opening scene, portal rendering and
player movement to continue in the observed test. The local test host uses
this entry in /Library/Application Support/RadeonNI/glthread:

  -hl2_launcher

This applies to Source games using that executable name. It disables Mesa's
extra CPU command worker, not hardware rendering, QE or CI. Treat it as a
failed workaround: Portal subsequently hung with glthread disabled too.
It is not a stability fix. RDN_SYNC is not enabled.

A separate exit hazard was found in user-client cleanup: syncForCPU only
flushes caches and does not drain submitted work. Cleanup now removes the
client's surface stores from the compositor registry and waits for the
submitted fence before freeing VRAM or unbinding GART pages. On timeout,
it retains the memory until reboot. clientclosetest passed 32 immediate
submit/close cycles on Leopard; a forced Portal exit also returned the GPU
to idle without freezing the desktop. Further gameplay testing is required.

On 10.5.9, OpenGL windows and full screen were tested with Tux Racer,
Quake 3 (timedemo four, 1920x1080: 185 fps), Pirates!, and the Doom 3 demo
menus. QE and Core Image remained in software in that earlier test.

The hardware cursor is not enabled. Sleep and display sleep have not been
checked. Other Power Mac models have not been tried.

Install (Terminal, in this folder, administrator password needed). Turn on
Remote Login (System Preferences, Sharing) first so that you can get in
over ssh if the screen stays dark.

  sudo sh ./install.sh --check      writes and loads nothing
  sudo sh ./install.sh --accel      kext + OpenGL bundle + 2D plug-in
                                    + microcode; then restart

Undo: sudo sh ./uninstall.sh, or Safe Boot (Shift held at startup), or
single-user mode (Command-S):
  /sbin/fsck -fy && /sbin/mount -uw /
  rm -rf /System/Library/Extensions/RadeonNI.kext
  rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
  reboot

Games and screensavers: Leopard's screensaver and sleep timers cover a game
that is waiting for you; set them to "Never" while testing.

What to send back: "sw_vers", "uname -a", the Mac model ("system_profiler
SPHardwareDataType"), the lines of /var/log/system.log that start with
"RadeonNI:", and what the monitor showed. Programs that start from the
Terminal write the bundle's log if RDN_GLD_LOG=/tmp/rdngld.log is set.

The driver is MIT-licensed (LICENSE); the microcode is AMD's (LICENSE.radeon).
Nothing is ever written to the card's flash.

Modern OpenGL clients
--------------------
The driver exports RDNGLRequestCompatibilityContext(major, minor),
RDNGLMakeCurrent(cgl_context), and RDNGLGetProcAddress(name). Call the request
function for 4.6 before creating any CGL contexts. It returns zero if a 4.6
compatibility context cannot be created, an invalid version was requested,
or CGL contexts already exist. A successful request applies to this process
only; ordinary desktop applications retain the Leopard compatibility path.

After each successful CGL/SDL context switch, call RDNGLMakeCurrent with the
current CGL context. A nonzero result verifies a bound Mesa 4.6 context.
Passing NULL unbinds Mesa. Function lookup requires a bound driver context.
Keep existing Apple entry points for drawable-aware operations, including
viewport, scissor, framebuffer binding, flush and presentation. Use the
bridge for entry points missing from Apple's framework. Core framebuffer
names must use the driver's EXT adapters when those are available.

tests/modern_context.c checks opt-in, immutable-buffer readback, GLSL 4.60
compute, thread handoff, and (with its optional 'legacy' argument) unchanged
legacy version exposure. This does not qualify all OpenGL 4.6 features.
