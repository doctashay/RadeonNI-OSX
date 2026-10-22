"""Exercise production fixture methods with real packers and a bounded fake GPU."""
import os
import pathlib
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class FixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "kext/RadeonNI/RadeonNIUvdFixture.cpp").read_text()
        source = source[source.index("enum { kFixturePage"):]
        harness = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <strings.h>
extern "C" {
#include "rdn_user.h"
#include "rdn_uvd.h"
#include "rdn_mem.h"
#include "rdn_uvd_h264.h"
#include "uvd_decode_fixture.h"
}
using UInt8 = uint8_t;
using UInt32 = uint32_t;
using SInt32 = int32_t;
using IOByteCount = size_t;
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotReady,
       kIOReturnNotPermitted, kIOReturnNotPrivileged };
static int truth;
static void *kOSBooleanTrue = &truth;
#define RDN_UVD_REBOOT_PROPERTY "RadeonNIUVDRebootRequired"
struct Service {
    bool enabled = true;
    void *getProperty(const char *) { return enabled ? kOSBooleanTrue : nullptr; }
};
using IOLock = std::mutex;
void IOLockLock(IOLock *lock) { lock->lock(); }
void IOLockUnlock(IOLock *lock) { lock->unlock(); }
struct Calls {
    int alloc = 0, freed = 0, cpuAlloc = 0, cpuFree = 0, emit = 0, sleeps = 0;
    int wake = 0, submits = 0, waits = 0, idle = 0, flush = 0;
    int failSubmit = 0, failWait = 0, emitError = 0, wakeError = 0, idleError = 0;
    bool allocFail = false, cpuFail = false, gfxTimeout = false, corrupt = false;
    bool untouched = false, inFlight = false;
    uint64_t allocation = 0x400000;
    uint64_t ibAddress = 0, messageAddress = 0, targetAddress = 0, feedbackAddress = 0;
    std::vector<uint8_t> ibCopy, messageCopy;
    std::vector<std::string> events;
} calls;
void *IOMalloc(size_t n) { ++calls.cpuAlloc; return calls.cpuFail ? nullptr : malloc(n); }
void IOFree(void *p, size_t) { ++calls.cpuFree; free(p); }
void IOSleep(unsigned) { ++calls.sleeps; }
void IOLog(const char *, ...) {}
static struct rdn_accel *active;
class RadeonNIAccel {
public:
    Service fb, provider;
    Service *fFramebuffer = &fb, *fUvdProvider = &provider;
    IOLock mutex, *fLock = &mutex;
    IOLock uvdMutex, *fUvdLock = &uvdMutex;
    bool fVideoBusy = false;
    bool fRetiring = false, fEngineUp = true;
    struct rdn_accel fAccel = {};
    struct rdn_mem fMem = {};
    struct rdn_uvd fUvd = {};
    UInt32 fUvdState = RDN_UVD_DIAGNOSTIC_RING_READY;
    SInt32 fUvdError = 0;
    struct rdn_user_uvd_fixture fUvdFixture = {};
    UInt32 fUvdFixtureOffset = 0, fUvdFixtureBytes = 0;
    UInt8 *fUvdFixtureSnapshot = nullptr;
    std::vector<uint32_t> memory;
    RadeonNIAccel() : memory(32 * 1024 * 1024 / 4, 0x33333333) {
        calls = Calls();
        fAccel.aperture = memory.data(); fAccel.aperture_size = memory.size() * 4;
        fUvd.ready = true; fUvd.swapped = true; fUvd.vcpu_gpu = 0x100000;
        fUvd.vcpu_bytes = 0x80000; fUvd.ring_gpu = 0x200000; fUvd.ring_words = 1024;
        active = &fAccel;
    }
    ~RadeonNIAccel() { if (fUvdFixtureSnapshot) free(fUvdFixtureSnapshot); }
    IOReturn uvdFixtureRun(struct rdn_user_uvd_fixture *);
    IOReturn uvdFixtureRead(UInt32, UInt32, void *);
    void releaseUvdFixtureSnapshot();
};
class RadeonNIUserClient {
public:
    RadeonNIAccel *fAccel;
    bool fUvdAdministrator = true;
    explicit RadeonNIUserClient(RadeonNIAccel *a) : fAccel(a) {}
    IOReturn methodUvdFixtureRun(UInt32, struct rdn_user_uvd_fixture *, IOByteCount *);
    IOReturn methodUvdFixtureRead(UInt32, UInt32, void *, IOByteCount *);
};
static uint8_t *at(uint64_t address) {
    assert(address >= active->vram_base);
    uint64_t offset = address - active->vram_base;
    assert(offset < active->aperture_size);
    return (uint8_t *)active->aperture + offset;
}
static uint32_t word(const uint8_t *p, bool be) {
    return be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
              : rdn_get_le32(p, 0);
}
extern "C" int rdn_mem_alloc(struct rdn_mem *, uint64_t bytes, uint64_t align, uint64_t *out) {
    ++calls.alloc; assert(bytes == 16728064 && align == 4096);
    if (calls.allocFail) return -ENOMEM;
    *out = calls.allocation; return 0;
}
extern "C" void rdn_mem_free(struct rdn_mem *, uint64_t offset) {
    assert(offset == calls.allocation && !calls.inFlight); ++calls.freed;
}
extern "C" int rdn_fence_emit(struct rdn_accel *, uint32_t *fence) {
    ++calls.emit; *fence = 42; return calls.emitError;
}
extern "C" bool rdn_fence_done(struct rdn_accel *, uint32_t fence) {
    assert(fence == 42); return !calls.gfxTimeout;
}
extern "C" void rdn_hdp_flush(struct rdn_accel *) { ++calls.flush; }
extern "C" int rdn_uvd_wake(struct rdn_uvd *) {
    ++calls.wake; calls.events.push_back("wake"); return calls.wakeError;
}
extern "C" int rdn_uvd_ib_submit(struct rdn_uvd *uvd, uint64_t address,
                                uint32_t count, uint32_t *sequence) {
    assert(!calls.inFlight); ++calls.submits;
    unsigned type = calls.submits - 1;
    assert(type <= 2 && count == (type == 1 ? 32u : 16u));
    uint8_t *ib = at(address);
    assert(word(ib, uvd->swapped) == (0xef10u >> 2));
    uint64_t message = word(ib + 4, uvd->swapped) | ((uint64_t)word(ib + 12, uvd->swapped) << 32);
    assert(rdn_get_le32(at(message), 0) == 3556 && rdn_get_le32(at(message), 4) == type);
    assert(rdn_get_le32(at(message), 8) == 1);
    if (type == 1) {
        assert(rdn_get_le32(at(message), 52) == 864);
        calls.targetAddress = word(ib + 19 * 4, uvd->swapped) |
            ((uint64_t)word(ib + 21 * 4, uvd->swapped) << 32);
        calls.feedbackAddress = word(ib + 25 * 4, uvd->swapped) |
            ((uint64_t)word(ib + 27 * 4, uvd->swapped) << 32);
        assert(rdn_get_le32(at(calls.feedbackAddress), 0) == 2048);
        uint64_t bitstream = word(ib + 13 * 4, uvd->swapped) |
            ((uint64_t)word(ib + 15 * 4, uvd->swapped) << 32);
        assert(!memcmp(at(bitstream), rdn_uvd_fixture_bitstream, 1408));
        for (unsigned i = 1408; i < 4096; ++i) assert(at(bitstream)[i] == 0);
    }
    calls.ibAddress = address; calls.messageAddress = message;
    calls.ibCopy.assign(ib, ib + count * 4);
    calls.messageCopy.assign(at(message), at(message) + 3568);
    calls.events.push_back("submit" + std::to_string(type));
    calls.inFlight = true;
    *sequence = ++uvd->fence_emitted;
    return calls.failSubmit == calls.submits ? -EIO : 0;
}
extern "C" int rdn_uvd_fence_wait(struct rdn_uvd *, uint32_t sequence, uint32_t timeout) {
    assert(calls.inFlight && sequence == (unsigned)calls.submits && timeout == 1000);
    ++calls.waits;
    assert(!memcmp(at(calls.ibAddress), calls.ibCopy.data(), calls.ibCopy.size()));
    assert(!memcmp(at(calls.messageAddress), calls.messageCopy.data(), calls.messageCopy.size()));
    calls.events.push_back("wait" + std::to_string(calls.submits - 1));
    if (calls.failWait == calls.waits) return -ETIMEDOUT;
    if (calls.submits == 2) {
        if (!calls.untouched)
            for (unsigned i = 0; i < 737280; ++i) at(calls.targetAddress)[i] = (uint8_t)(i * 13 + 7);
        for (unsigned i = 0; i < 2048; ++i) at(calls.feedbackAddress)[i] = (uint8_t)(i ^ 0x69);
        if (calls.corrupt) at(calls.targetAddress)[737280] = 0;
    }
    calls.inFlight = false;
    return 0;
}
extern "C" int rdn_uvd_idle(struct rdn_uvd *) {
    assert(!calls.inFlight && calls.waits == 3); ++calls.idle; return calls.idleError;
}
'''
        harness += source
        harness += r'''
static void success(bool be, bool idleFailure) {
    RadeonNIAccel a; a.fUvd.swapped = be; calls.idleError = idleFailure ? -EIO : 0;
    struct rdn_user_uvd_fixture result = {};
    assert(a.uvdFixtureRun(&result) == 0 && result.state == RDN_UVD_FIXTURE_COMPLETE);
    assert(!result.error && result.idle_error == calls.idleError);
    assert(result.flags == (RDN_UVD_FIXTURE_OUTPUT_READY | RDN_UVD_FIXTURE_GUARDS_OK));
    assert(calls.submits == 3 && calls.waits == 3 && !calls.freed);
    std::vector<std::string> expected = {"wake","submit0","wait0","submit1","wait1","submit2","wait2"};
    assert(calls.events == expected);
    assert(a.fUvdFixtureBytes == 16728064 && a.fUvdFixtureOffset == calls.allocation);
    assert(a.uvdFixtureRun(&result) == 0 && calls.submits == 3 && calls.alloc == 1);
    uint8_t block[4096];
    assert(a.uvdFixtureRead(1, 0, block) == 0);
    for (unsigned i = 0; i < sizeof(block); ++i) assert(block[i] == (uint8_t)(i * 13 + 7));
    assert(a.uvdFixtureRead(1, 180, block) == 0);
    for (unsigned i = 0; i < 2048; ++i) assert(block[i] == (uint8_t)(i ^ 0x69));
    for (unsigned i = 2048; i < sizeof(block); ++i) assert(!block[i]);
    assert(a.uvdFixtureRead(2, 0, block) == kIOReturnBadArgument);
    for (unsigned i = 0; i < sizeof(block); ++i) assert(!block[i]);
    assert(a.uvdFixtureRead(1, 181, block) == kIOReturnBadArgument);
    assert(a.uvdFixtureRead(1, UINT32_MAX, block) == kIOReturnBadArgument);
    RadeonNIUserClient reconnect(&a); IOByteCount size = sizeof(block);
    assert(reconnect.methodUvdFixtureRead(1, 0, block, &size) == 0);
    a.fRetiring = true;
    assert(a.uvdFixtureRead(1, 0, block) == kIOReturnNotReady);
    assert(!calls.freed && !calls.cpuFree);
    a.releaseUvdFixtureSnapshot(); a.releaseUvdFixtureSnapshot();
    assert(calls.cpuFree==1 && !calls.freed && a.fUvdFixtureBytes==16728064);
    assert(!a.fUvdFixtureSnapshot && !(a.fUvdFixture.flags & RDN_UVD_FIXTURE_OUTPUT_READY));
}
static void gate(unsigned n) {
    RadeonNIAccel a; struct rdn_user_uvd_fixture r = {};
    switch(n) {
    case 0: a.fRetiring=true; break; case 1: a.fFramebuffer=nullptr; break;
    case 2: a.fb.enabled=false; break; case 3: a.fEngineUp=false; break;
    case 4: a.fUvdState=RDN_UVD_DIAGNOSTIC_OFF; break; case 5: a.fUvd.ready=false; break;
    case 6: a.fUvd.failed=true; break; case 7: a.fUvd.stopped=true; break;
    case 8: a.provider.enabled=false; break; case 9: a.fUvdProvider=nullptr; break;
    case 10: a.fAccel.aperture=nullptr; break; default: assert(false);
    }
    assert(a.uvdFixtureRun(&r) != 0 && !calls.alloc && !calls.submits && !calls.wake);
}
static void preflight(unsigned n) {
    RadeonNIAccel a; struct rdn_user_uvd_fixture r = {};
    switch(n) {
    case 0: calls.allocFail=true; break;
    case 1: calls.allocation=a.fAccel.aperture_size; break;
    case 2: calls.allocation=UINT64_MAX; break;
    case 3: calls.allocation=0x100000; break;
    case 4: calls.allocation=0x200000; break;
    case 5: a.fUvd.vcpu_gpu=0x10000000; break;
    case 6: a.fAccel.vram_base=(1ull<<40)-4096; break;
    case 7: calls.cpuFail=true; break;
    case 8: calls.emitError=-EIO; break;
    case 9: calls.gfxTimeout=true; break;
    case 10: calls.wakeError=-EIO; break;
    case 11: a.fUvd.ring_words=UINT32_MAX; break;
    case 12: calls.allocation=0x180000-4096; break; // Guard page overlaps VCPU.
    default: assert(false);
    }
    assert(a.uvdFixtureRun(&r) == 0 && r.state == RDN_UVD_FIXTURE_FAILED);
    assert(r.error && !calls.submits && !a.fUvdFixtureBytes && !a.fUvdFixtureSnapshot);
    assert(calls.freed == ((n==0 || n==11) ? 0 : 1));
    if(n==9) assert(calls.sleeps==1000);
    assert(a.memory[0] == 0x33333333 && a.memory.back() == 0x33333333);
    assert(a.uvdFixtureRun(&r)==0 && !calls.submits);
}
static void submittedFailure(unsigned n) {
    RadeonNIAccel a; struct rdn_user_uvd_fixture r = {};
    if(n<3) calls.failSubmit=n+1;
    else if(n<6) calls.failWait=n-2;
    else if(n==6) calls.corrupt=true;
    else if(n==7) calls.untouched=true;
    else assert(false);
    assert(a.uvdFixtureRun(&r)==0 && r.state==RDN_UVD_FIXTURE_FAILED && r.error);
    assert(!calls.freed && a.fUvdFixtureBytes==16728064);
    assert(a.fUvdState==RDN_UVD_DIAGNOSTIC_FAULTED && !a.fUvd.ready);
    unsigned submitted=calls.submits;
    assert(a.uvdFixtureRun(&r)==0 && (unsigned)calls.submits==submitted);
    uint8_t block[4096]; memset(block,0xcc,sizeof(block));
    if(n<6) {
        assert(!a.fUvdFixtureSnapshot && !(r.flags & RDN_UVD_FIXTURE_OUTPUT_READY));
        assert(a.uvdFixtureRead(1,0,block)==kIOReturnNotReady);
        for(unsigned i=0;i<sizeof(block);++i) assert(!block[i]);
    } else {
        assert(a.fUvdFixtureSnapshot && (r.flags & RDN_UVD_FIXTURE_OUTPUT_READY));
        assert(a.uvdFixtureRead(1,0,block)==0);
        for(unsigned i=0;i<sizeof(block);++i)
            assert(block[i]==(n==7 ? 0xcd : (uint8_t)(i*13+7)));
        assert(a.uvdFixtureRead(1,180,block)==0);
        for(unsigned i=0;i<2048;++i) assert(block[i]==(uint8_t)(i^0x69));
        for(unsigned i=2048;i<sizeof(block);++i) assert(!block[i]);
        assert(!calls.idle);
    }
    if(n<3) assert(submitted==n+1);
    else if(n<6) assert(submitted==n-2);
}
static void guards() {
    RadeonNIAccel a; RadeonNIUserClient u(&a); struct rdn_user_uvd_fixture r;
    IOByteCount size=sizeof(r); uint8_t data[4096];
    assert(u.methodUvdFixtureRun(1,&r,&size)==kIOReturnBadArgument);
    assert(u.methodUvdFixtureRun(0,nullptr,&size)==kIOReturnBadArgument);
    assert(u.methodUvdFixtureRun(0,&r,nullptr)==kIOReturnBadArgument);
    --size; assert(u.methodUvdFixtureRun(0,&r,&size)==kIOReturnBadArgument);
    size=sizeof(r); u.fUvdAdministrator=false; memset(&r,0xff,sizeof(r));
    assert(u.methodUvdFixtureRun(0,&r,&size)==kIOReturnNotPrivileged);
    for(unsigned i=0;i<sizeof(r);++i) assert(!((uint8_t*)&r)[i]);
    size=sizeof(data); memset(data,0xff,sizeof(data));
    assert(u.methodUvdFixtureRead(1,0,data,&size)==kIOReturnNotPrivileged);
    for(unsigned i=0;i<sizeof(data);++i) assert(!data[i]);
    --size; assert(u.methodUvdFixtureRead(1,0,data,&size)==kIOReturnBadArgument);
    assert(!calls.alloc && !calls.submits);
    assert(RDN_UC_UVD_DIAGNOSTIC_INFO==22 && RDN_UC_UVD_DIAGNOSTIC_INIT==23);
    assert(RDN_UC_UVD_FIXTURE_RUN==24 && RDN_UC_UVD_FIXTURE_READ==25);
    assert(sizeof(r)==96 && offsetof(rdn_user_uvd_fixture,reserved)==76);
}
static void concurrent() {
    RadeonNIAccel a; rdn_user_uvd_fixture x={},y={};
    std::thread first([&]{ assert(a.uvdFixtureRun(&x)==0); });
    std::thread second([&]{ assert(a.uvdFixtureRun(&y)==0); });
    first.join(); second.join();
    assert(!memcmp(&x,&y,sizeof(x)) && calls.alloc==1 && calls.submits==3);
}
int main(int argc,char **argv) {
    assert(argc==3); std::string name=argv[1]; unsigned n=strtoul(argv[2],nullptr,10);
    if(name=="success") success(n!=0,false);
    else if(name=="idle") success(true,true);
    else if(name=="gate") gate(n);
    else if(name=="preflight") preflight(n);
    else if(name=="submitted") submittedFailure(n);
    else if(name=="guards") guards();
    else if(name=="concurrent") concurrent();
    else assert(false);
}
'''
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = pathlib.Path(cls.directory.name)
        (path / "test.cpp").write_text(harness)
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = shlex.split(os.environ.get("UVD_FIXTURE_FLAGS", "-O2"))
        cls.runner = shlex.split(os.environ.get("UVD_FIXTURE_RUNNER", ""))
        include = ["-I", str(ROOT / "hw"), "-I", str(ROOT / "tests")]
        objects = []
        for source in [ROOT / "hw/rdn_uvd_h264.c", ROOT / "tests/uvd_decode_fixture.c"]:
            obj = path / (source.stem + ".o")
            subprocess.run(cc + ["-std=c99", "-Wall", "-Wextra", "-Werror"] + flags +
                           include + ["-c", str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        cls.exe = path / "test"
        subprocess.run(cxx + ["-std=c++11", "-Wall", "-Wextra", "-Werror"] + flags + include +
                       [str(path / "test.cpp")] + objects + ["-pthread", "-o", str(cls.exe)], check=True)

    def cases(self, kind, count=1):
        for number in range(count):
            with self.subTest(number=number):
                subprocess.run(self.runner + [str(self.exe), kind, str(number)], check=True, timeout=30)

    def test_both_ib_orders_snapshot_and_reconnect(self): self.cases("success", 2)
    def test_idle_failure_keeps_completed_snapshot(self): self.cases("idle")
    def test_gates_have_no_allocation_or_submission(self): self.cases("gate", 11)
    def test_prehardware_failures_release_only_unsubmitted_storage(self): self.cases("preflight", 13)
    def test_every_submission_failure_retains_storage(self): self.cases("submitted", 8)
    def test_admin_input_zeroing_and_stable_abi(self): self.cases("guards")
    def test_concurrent_runs_submit_once(self): self.cases("concurrent")


if __name__ == "__main__":
    unittest.main()
