"""Exercise kernel mirror submission with delayed GPU completion."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
using UInt32=uint32_t; using UInt64=uint64_t; using UInt8=uint8_t;
using IOReturn=int;
enum {kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNoMemory,
      kIOReturnNotReady, kIOReturnNoResources, kIOReturnTimeout, kIOReturnBusy};
static unsigned waits, frees, allocations, copies, locks;
static bool failSubmit, failAlloc;
void *IOMalloc(unsigned n){return malloc(n);}
void IOFree(void *p,unsigned){free(p);}
void IOLockLock(void*){assert(!locks);++locks;}
void IOLockUnlock(void*){assert(locks==1);--locks;}
void rdnStoreBarrier(){}
struct rdn_mem{};
struct Accel {void *aperture; unsigned fence_emitted=7;};
static uint64_t rdn_vram_addr(Accel*,uint64_t n){return n;}
static int rdn_mem_alloc(rdn_mem*,unsigned,unsigned,uint64_t *n){++allocations;*n=4096;return failAlloc;}
static void rdn_mem_free(rdn_mem*,uint64_t){++frees;}
static int rdn_cp_dma_surface_build(uint64_t,unsigned,uint64_t,unsigned,unsigned,unsigned,UInt32 *p,unsigned,uint32_t *words){p[0]=0;*words=16;return 0;}
static int rdn_ib_submit(Accel *a,uint64_t,unsigned,uint32_t *seq){++copies;if(failSubmit)return 1;*seq=++a->fence_emitted;return 0;}
static int rdn_fence_wait(Accel*,unsigned,unsigned){++waits;return 1;}
struct RetiredVram {unsigned offset,fence;RetiredVram *next;};
class RadeonNIAccel {
public:
 enum {kSurfaceMirrors=4};
 struct SurfaceMirror {unsigned surface=0,offset=0,stride=0,width=0,height=0;} fSurfaceMirrors[4];
 struct SurfaceRecord {void *owner=nullptr;unsigned bufOffset=0,bufRowBytes=0,bufWidth=0,bufHeight=0;} fShapes[4];
 unsigned char aperture[8192]{};Accel fAccel{aperture};
 rdn_mem fMem,fCommandMem;RetiredVram *fRetiredVram=nullptr;
 bool fRetiring=false,fSurfaceGpuStalled=false;void *fLock=nullptr;
 int surfaceSlot(unsigned n,bool){return n<4?(int)n:-1;}
 uint64_t surfaceVramAddress(unsigned n){return n;}
 void setProperty(const char*,bool){assert(false);}
 void reapRetiredVram(){}
 IOReturn surfaceGpuTransfer(UInt64,UInt32,UInt64,UInt32,UInt32,UInt32,UInt32,bool,bool);
 IOReturn refreshSurfaceGpuOwner(void*,UInt32*);
};
const bool kOSBooleanTrue=true;
'''
CASES = r'''
int main(){
 RadeonNIAccel a;int owner;unsigned fence=7;
 auto &s=a.fShapes[1];s.owner=&owner;s.bufOffset=100;s.bufRowBytes=256;s.bufWidth=s.bufHeight=64;
 auto &m=a.fSurfaceMirrors[0];m.surface=1;m.offset=100;m.stride=256;m.width=m.height=64;
 assert(!a.refreshSurfaceGpuOwner(&owner,&fence));
 assert(!waits&&!copies&&!allocations&&!a.fSurfaceGpuStalled&&fence==7);
 m.offset=200;
 assert(!a.refreshSurfaceGpuOwner(&owner,&fence));
 assert(!waits&&!frees&&copies==1&&fence==8&&!a.fSurfaceGpuStalled);
 assert(a.fRetiredVram&&a.fRetiredVram->offset==4096&&a.fRetiredVram->fence==8);
 auto *pending=a.fRetiredVram;failSubmit=true;
 assert(a.refreshSurfaceGpuOwner(&owner,&fence)==kIOReturnBusy);
 assert(a.fRetiredVram==pending&&frees==1&&fence==8&&!waits&&!a.fSurfaceGpuStalled);
 failSubmit=false;failAlloc=true;
 assert(a.refreshSurfaceGpuOwner(&owner,&fence)==kIOReturnNoResources);
 assert(a.fRetiredVram==pending&&frees==1&&fence==8&&!locks);
 free(pending);
 puts("PASS self mirrors never wait; queued copies retain commands and extend completion; failures publish nothing");
}
'''


class MirrorQueue(unittest.TestCase):
    def test_delayed_completion(self):
        source = (ROOT / 'kext/RadeonNI/RadeonNIAccel.cpp').read_text()
        transfer = source[source.index('IOReturn RadeonNIAccel::surfaceGpuTransfer('):
                          source.index('IOReturn RadeonNIAccel::syncSurfaceGpu(')]
        refresh = source[source.index('IOReturn RadeonNIAccel::refreshSurfaceGpuOwner('):
                         source.index('IOReturn RadeonNIUserClient::methodSurfaceGpuInfo(')]
        with tempfile.TemporaryDirectory() as directory:
            p = pathlib.Path(directory)
            (p / 'test.cpp').write_text(HARNESS + transfer + refresh + CASES)
            subprocess.run(['c++', '-std=c++17', str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()

