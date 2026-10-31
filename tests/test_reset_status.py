"""Check graphics fault reporting without treating pending fences as resets."""
import os
import pathlib
import shlex
import subprocess
import tempfile
import unittest
from test_client_cleanup import function

ROOT = pathlib.Path(__file__).resolve().parents[1]

class ResetStatusTest(unittest.TestCase):
    def test_status(self):
        source = (ROOT / "mesa/winsys/rdn_winsys.c").read_text(encoding="utf-8")
        method = function(source, "static enum pipe_reset_status\nrdn_ctx_query_reset_status(")
        prefix = r"""
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
enum pipe_reset_status {PIPE_NO_RESET, PIPE_UNKNOWN_CONTEXT_RESET};
struct radeon_winsys_ctx;
struct rdn_device {int (*graphics_fault)(struct rdn_device *);};
struct radeon_drm_winsys {struct rdn_device *dev;};
static int state;
static int query(struct rdn_device *dev) {return state;}
"""
        main = r"""
int main(void) {
 struct rdn_device dev = {query};
 struct radeon_drm_winsys ws = {&dev};
 struct radeon_winsys_ctx *ctx = (void *)&ws;
 for (state = -1; state <= 1; ++state) {
  for (int full = 0; full < 2; ++full) {
   bool needed = true, completed = true;
   enum pipe_reset_status expected = state == 1 ? PIPE_UNKNOWN_CONTEXT_RESET : PIPE_NO_RESET;
   assert(rdn_ctx_query_reset_status(ctx, full, &needed, &completed) == expected);
   assert(needed == (state == 1) && !completed);
   assert(rdn_ctx_query_reset_status(ctx, full, NULL, NULL) == expected);
  }
 }
 dev.graphics_fault = NULL;
 bool needed = true, completed = true;
 assert(rdn_ctx_query_reset_status(ctx, false, &needed, &completed) == PIPE_NO_RESET);
 assert(!needed && !completed);
 puts("PASS graphics faults are reported without inventing recovery or guilt");
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(prefix + method + main, encoding="utf-8")
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter", str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run(shlex.split(os.environ.get("TEST_RUNNER", "")) + [str(path / "test")], check=True)

    def test_kernel_fault_flag(self):
        source = (ROOT / "kext/RadeonNI/RadeonNIFence.cpp").read_text(encoding="utf-8")
        method = function(source, "IOReturn RadeonNIAccel::fenceControl(")
        prefix = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
using UInt32 = uint32_t;
using IOReturn = int;
enum {kIOReturnSuccess, kIOReturnBusy, kIOReturnBadArgument};
void IOLockLock(int) {}
void IOLockUnlock(int) {}
struct RadeonNIAccel {
 int fLock=0, fFenceControlLock=0;
 bool fFenceIRQEnabled=false, fSurfaceGpuStalled=false;
 UInt32 fFenceEvents=7, fFenceFallbacks=9;
 bool waitForCPU(){return false;}
 int startFenceInterrupts(){fFenceIRQEnabled=true;return 0;}
 void stopFenceInterrupts(){fFenceIRQEnabled=false;}
 IOReturn fenceControl(UInt32,UInt32*,UInt32*,UInt32*);
};
"""
        main = r"""
int main() {
 RadeonNIAccel a;
 for(unsigned fault=0;fault<2;++fault)for(unsigned irq=0;irq<2;++irq){
  a.fSurfaceGpuStalled=fault;a.fFenceIRQEnabled=irq;
  UInt32 flags=0,events=0,fallbacks=0;
  assert(!a.fenceControl(0,&flags,&events,&fallbacks));
  assert(flags==(2u|irq|(fault?4u:0u)) && events==7 && fallbacks==9);
 }
 puts("PASS fault query preserves IRQ flags and counters");
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.cpp").write_text(prefix + method + main, encoding="utf-8")
            subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++11", str(path / "test.cpp"), "-o", str(path / "test")], check=True)
            subprocess.run(shlex.split(os.environ.get("TEST_RUNNER", "")) + [str(path / "test")], check=True)

if __name__ == "__main__":
    unittest.main()
