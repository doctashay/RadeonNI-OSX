"""Exercise the production UVD diagnostic methods without a device or firmware."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


class UvdDiagnosticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "kext/RadeonNI/RadeonNIAccel.cpp").read_text()
        header = (ROOT / "kext/RadeonNI/RadeonNIAccel.h").read_text()
        marker = next(line for line in header.splitlines()
                      if line.startswith("#define RDN_UVD_REBOOT_PROPERTY "))
        harness = marker + "\n" + r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <condition_variable>
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
}
using UInt32 = uint32_t;
using SInt32 = int32_t;
using IOByteCount = size_t;
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnError, kIOReturnNotPermitted,
       kIOReturnNotReady, kIOReturnNotFound, kIOReturnBadArgument,
       kIOReturnNoMemory, kIOReturnNotPrivileged, kIOReturnTimeout };
struct OSObject { virtual ~OSObject() {} } booleanTrue;
OSObject *kOSBooleanTrue = &booleanTrue;
struct OSData : OSObject {
    UInt32 length = 4096;
    mutable std::vector<uint8_t> bytes;
    UInt32 getLength() const { return length; }
    const void *getBytesNoCopy() const { bytes.resize(length, 0x5a); return bytes.data(); }
};
#define OSDynamicCast(type, p) dynamic_cast<type *>(p)
bool validMarker = false;
struct Provider {
    OSObject *marker = nullptr;
    bool failSet = false, dropSet = false;
    int sets = 0, removes = 0;
    OSObject *getProperty(const char *name) {
        assert(!strcmp(name, RDN_UVD_REBOOT_PROPERTY)); return marker;
    }
    bool setProperty(const char *name, bool value) {
        assert(!strcmp(name, RDN_UVD_REBOOT_PROPERTY) && value); ++sets;
        if (failSet) return false;
        if (!dropSet) { marker = kOSBooleanTrue; validMarker = true; }
        return true;
    }
    void removeProperty(const char *name) {
        assert(!strcmp(name, RDN_UVD_REBOOT_PROPERTY));
        ++removes; marker = nullptr; validMarker = false;
    }
};
struct Framebuffer {
    bool allowed = true, firmwarePresent = true, rlcPresent = true;
    OSData firmware, rlc;
    Framebuffer() { rlc.length = 3072; }
    OSObject *getProperty(const char *name) {
        if (!strcmp(name, "EnableUVDDiagnostics")) return allowed ? kOSBooleanTrue : nullptr;
        if (!strcmp(name, "FW_UVD")) return firmwarePresent ? &firmware : nullptr;
        assert(!strcmp(name, "FW_RLC"));
        return rlcPresent ? &rlc : nullptr;
    }
};
struct Calls {
    int firmwareAlloc = 0, firmwareFree = 0, alloc = 0, freed = 0;
    int refClock = 0, init = 0, sizing = 0, prepare = 0, start = 0;
    int emit = 0, done = 0, sleeps = 0, flush = 0, submit = 0;
    int emitError = 0, completeAfterSleeps = 0;
    uint32_t emittedSeq = 0;
    bool allocFailure = false, startedFailure = false, prehardwareFailure = false;
    uint64_t offset = 4096, freedOffset = 0;
} calls;
std::mutex gateMutex;
std::condition_variable gate;
std::atomic<int> lockAttempts{0};
bool holdInit = false, initEntered = false, releaseInit = false;
bool holdFence = false, fenceEntered = false, releaseFence = false;
thread_local int lockDepth = 0;
static void IOLockLock(std::mutex *lock) {
    { std::lock_guard<std::mutex> guard(gateMutex); if (!lockDepth) ++lockAttempts; }
    gate.notify_all();
    lock->lock(); ++lockDepth;
}
static void IOLockUnlock(std::mutex *lock) { assert(lockDepth > 0); --lockDepth; lock->unlock(); }
static void *IOMalloc(size_t size) { ++calls.firmwareAlloc; return malloc(size); }
static void IOFree(void *p, size_t) { ++calls.firmwareFree; free(p); }
static void IOLog(const char *, ...) {}
static void IOSleep(unsigned ms) {
    assert(lockDepth == 2 && ms == 1); ++calls.sleeps;
    std::unique_lock<std::mutex> guard(gateMutex);
    fenceEntered = true; gate.notify_all();
    gate.wait(guard, [] { return !holdFence || releaseFence; });
}
extern "C" int rdn_fence_emit(rdn_accel *accel, uint32_t *seq) {
    assert(lockDepth == 2); ++calls.emit;
    if (calls.emitError) return calls.emitError;
    calls.emittedSeq = ++accel->fence_emitted; *seq = calls.emittedSeq;
    return 0;
}
extern "C" bool rdn_fence_done(rdn_accel *, uint32_t seq) {
    assert(lockDepth == 2 && calls.emit && seq == calls.emittedSeq); ++calls.done;
    return calls.sleeps >= calls.completeAfterSleeps;
}
extern "C" void rdn_hdp_flush(rdn_accel *) {
    assert(lockDepth == 2 && calls.done && calls.sleeps >= calls.completeAfterSleeps);
    ++calls.flush;
}
extern "C" uint32_t rdn_uvd_vcpu_bytes(uint32_t firmware, uint32_t handles) {
    ++calls.sizing;
    assert(handles == RDN_UVD_DEFAULT_HANDLES);
    return firmware ? 1024 * 1024 : 0;
}
extern "C" int rdn_uvd_ref_clock(rdn_card *, uint32_t *clock) {
    ++calls.refClock; *clock = 2700; return 0;
}
extern "C" int rdn_mem_alloc(rdn_mem *, uint64_t, uint64_t, uint64_t *offset) {
    ++calls.alloc; *offset = calls.offset; return calls.allocFailure ? -1 : 0;
}
extern "C" void rdn_mem_free(rdn_mem *, uint64_t offset) {
    ++calls.freed; calls.freedOffset = offset;
}
extern "C" bool rdn_uvd_storage_releasable(const rdn_uvd *uvd) { return !uvd->started; }
extern "C" int rdn_uvd_init_rlc(rdn_uvd *uvd, rdn_accel *, uint32_t clock,
    const uint8_t *, uint32_t, uint32_t, uint32_t vcpu, uint32_t ring,
    uint32_t ringBytes, uint32_t flags, const uint8_t *rlc, uint32_t rlcSize) {
    assert(lockDepth == 2 && calls.flush && !calls.submit);
    assert(rlc && rlcSize == 3072 && rlc[0] == 0x5a && rlc[3071] == 0x5a);
    ++calls.init; assert(validMarker); assert(!uvd->failed && !uvd->started); assert(clock == 2700 && ring > vcpu && ringBytes == 4096 && !flags);
    {
        std::unique_lock<std::mutex> guard(gateMutex);
        initEntered = true; gate.notify_all();
        gate.wait(guard, [] { return !holdInit || releaseInit; });
    }
    uvd->guarded = true;
    uvd->started = !calls.prehardwareFailure;
    uvd->failed = calls.startedFailure || calls.prehardwareFailure;
    uvd->status_snapshot = 0x1234abcd;
    return calls.startedFailure || calls.prehardwareFailure ? -5 : 0;
}
extern "C" int rdn_uvd_prepare_rlc(rdn_uvd *uvd, rdn_accel *, uint32_t clock,
    const uint8_t *, uint32_t, uint32_t, uint32_t vcpu, uint32_t ring,
    uint32_t ringBytes, uint32_t flags, const uint8_t *rlc, uint32_t rlcSize) {
    assert(lockDepth == 2 && !calls.emit && !calls.flush && !calls.submit);
    assert(validMarker && !uvd->started && !uvd->prepared);
    assert(clock == 2700 && ring > vcpu && ringBytes == 4096 && !flags);
    assert(rlc && rlcSize == 3072 && rlc[0] == 0x5a && rlc[3071] == 0x5a);
    ++calls.prepare;
    uvd->guarded = true;
    uvd->started = !calls.prehardwareFailure;
    uvd->failed = calls.startedFailure || calls.prehardwareFailure;
    uvd->prepared = !uvd->failed;
    uvd->status_snapshot = 0;
    return uvd->failed ? -5 : 0;
}
extern "C" int rdn_uvd_start_prepared(rdn_uvd *uvd) {
    assert(lockDepth == 2 && calls.flush && calls.prepare == 1);
    assert(validMarker && uvd->prepared && uvd->started && !uvd->failed);
    ++calls.start;
    uvd->prepared = false;
    uvd->failed = calls.startedFailure;
    uvd->status_snapshot = 0x1234abcd;
    return uvd->failed ? -5 : 0;
}
class RadeonNIAccel {
public:
    std::mutex mutex;
    std::mutex *fLock = &mutex;
    std::mutex uvdMutex, *fUvdLock = &uvdMutex;
    bool fVideoBusy = false, fUvdUp = false;
    UInt32 fUvdUsers=0,fUvdLive=0,fUvdEpoch=0,fUvdFlags=0,fUvdRef=0;
    Framebuffer framebuffer;
    Framebuffer *fFramebuffer = &framebuffer;
    bool fEngineUp = true, fRetiring = false, fUvdColdAttempted = false;
    Provider provider;
    Provider *fUvdProvider = &provider;
    rdn_accel fAccel{};
    rdn_mem fMem{};
    rdn_uvd fUvd{};
    UInt32 fUvdState = RDN_UVD_DIAGNOSTIC_OFF, fUvdOffset = 0, fUvdBytes = 0, fUvdStatus = 0;
    SInt32 fUvdError = 0;
    RadeonNIAccel() { fAccel.aperture_size = 8 * 1024 * 1024; }
    void prepareToRetire();
    void *copyFirmware(const char *, UInt32 *);
    void uvdDiagnosticInfo(rdn_user_uvd_diagnostic *);
    IOReturn uvdDiagnosticInit(UInt32 *);
    IOReturn uvdDiagnosticInitImpl(UInt32 *, bool);
    static void prepareUvdBeforeCP(void *);
};
class RadeonNIUserClient {
public:
    RadeonNIAccel *fAccel = nullptr;
    bool fUvdAdministrator = false;
    IOReturn methodUvdDiagnosticInfo(rdn_user_uvd_diagnostic *, IOByteCount *);
    IOReturn methodUvdDiagnosticInit(UInt32, UInt32 *);
};
'''
        for signature in (
            "void RadeonNIAccel::prepareToRetire(",
            "void *RadeonNIAccel::copyFirmware(",
            "void RadeonNIAccel::uvdDiagnosticInfo(",
            "void RadeonNIAccel::prepareUvdBeforeCP(",
            "IOReturn RadeonNIAccel::uvdDiagnosticInit(",
            "IOReturn RadeonNIAccel::uvdDiagnosticInitImpl(",
            "IOReturn RadeonNIUserClient::methodUvdDiagnosticInfo(",
            "IOReturn RadeonNIUserClient::methodUvdDiagnosticInit(",
        ):
            harness += function(source, signature) + "\n"
        harness += r'''
static void requireNoDeviceWork() {
    assert(!calls.alloc && !calls.init && !calls.refClock);
    assert(!calls.emit && !calls.done && !calls.sleeps && !calls.flush);
}
static void disabled() {
    RadeonNIAccel a; a.framebuffer.allowed = false;
    UInt32 status = 999;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotPermitted && !status);
    requireNoDeviceWork(); assert(!calls.firmwareAlloc);
}
static void missingFirmware() {
    RadeonNIAccel a; a.framebuffer.firmwarePresent = false; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotFound);
    requireNoDeviceWork(); assert(!calls.firmwareAlloc);
}

static void missingRlc() {
    RadeonNIAccel a; a.framebuffer.rlcPresent = false; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotFound);
    requireNoDeviceWork(); assert(calls.firmwareAlloc == 1 && calls.firmwareFree == 1);
}
static void malformedRlc() {
    for (UInt32 size : {1u, 3071u, 3073u, 4096u}) {
        RadeonNIAccel a; a.framebuffer.rlc.length = size; UInt32 status;
        assert(a.uvdDiagnosticInit(&status) == kIOReturnBadArgument);
        requireNoDeviceWork(); assert(calls.firmwareAlloc == calls.firmwareFree);
        assert(!a.provider.marker && !a.fUvdBytes);
    }
}
static void rlcQuery() {
    RadeonNIAccel a; rdn_user_uvd_diagnostic info;
    for (UInt32 size : {0u, 3071u, 3072u, 3073u}) {
        a.framebuffer.rlc.length = size; a.uvdDiagnosticInfo(&info);
        assert(bool(info.flags & RDN_UVD_DIAGNOSTIC_RLC_FIRMWARE) == (size == 3072));
    }
    a.framebuffer.rlcPresent = false; a.uvdDiagnosticInfo(&info);
    assert(!(info.flags & RDN_UVD_DIAGNOSTIC_RLC_FIRMWARE));
    requireNoDeviceWork(); assert(!calls.firmwareAlloc);
}
static void emitFailure() {
    RadeonNIAccel a; calls.emitError = -16; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady && !status);
    assert(a.fUvdError == -16 && calls.emit == 1 && !calls.done);
    assert(!calls.alloc && !calls.init && !a.provider.marker && !a.fUvdBytes);
    assert(calls.firmwareAlloc == calls.firmwareFree);
    calls.emitError = 0;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(calls.emit == 2 && calls.init == 1);
}
static void fenceTimeout() {
    RadeonNIAccel a; calls.completeAfterSleeps = 1001; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnTimeout && !status);
    assert(a.fUvdError == -ETIMEDOUT && calls.emit == 1 && calls.sleeps == 1000);
    assert(calls.done == 1001 && !calls.flush && !calls.alloc && !calls.init);
    assert(!a.provider.marker && !a.fUvdBytes && a.fUvdState == RDN_UVD_DIAGNOSTIC_OFF);
    assert(calls.firmwareAlloc == calls.firmwareFree);
    calls.completeAfterSleeps = 0;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(calls.emit == 2 && calls.init == 1);
}
static void fenceDeadline() {
    RadeonNIAccel a; a.fAccel.fence_emitted = 72;
    calls.completeAfterSleeps = 1000; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(calls.emittedSeq == 73 && calls.done == 1001 && calls.sleeps == 1000);
    assert(calls.flush == 1 && calls.init == 1);
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess && calls.emit == 1);
}
static void submitBlockedDuringIdle() {
    RadeonNIAccel a; holdFence = true; calls.completeAfterSleeps = 1;
    UInt32 status = 0; int result = -1; std::atomic<bool> submitted{false};
    std::thread init([&] { result = a.uvdDiagnosticInit(&status); });
    {
        std::unique_lock<std::mutex> guard(gateMutex);
        gate.wait(guard, [] { return fenceEntered; });
    }
    std::thread submit([&] {
        IOLockLock(a.fLock); ++calls.submit; submitted = true; IOLockUnlock(a.fLock);
    });
    {
        std::unique_lock<std::mutex> guard(gateMutex);
        gate.wait(guard, [] { return lockAttempts.load() == 2; });
        assert(!submitted.load()); releaseFence = true;
    }
    gate.notify_all(); init.join(); submit.join();
    assert(result == kIOReturnSuccess && submitted.load() && calls.submit == 1);
    assert(calls.emit == 1 && calls.init == 1);
}

static void invalidFirmware() {
    RadeonNIAccel a; a.framebuffer.firmware.length = (1u << 20) + 1; UInt32 status;
    // The size gate must precede clock or VRAM operations.
    assert(a.uvdDiagnosticInit(&status) == kIOReturnBadArgument);
    requireNoDeviceWork(); assert(calls.firmwareAlloc == 2 && calls.firmwareFree == 2);
}
static void allocationFailure() {
    RadeonNIAccel a; calls.allocFailure = true; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNoMemory);
    assert(calls.alloc == 1 && !calls.init && !calls.freed && !a.fUvdBytes);
    assert(calls.firmwareAlloc == calls.firmwareFree);
}
static void allocationRange() {
    for (uint64_t offset : {uint64_t(8 * 1024 * 1024 - 4096), uint64_t(1) << 32}) {
        RadeonNIAccel a; calls.offset = offset; UInt32 status;
        int before = calls.freed;
        assert(a.uvdDiagnosticInit(&status) == kIOReturnBadArgument);
        assert(calls.freed == before + 1 && calls.freedOffset == offset);
        assert(!calls.init && !a.fUvdBytes && !a.fUvdOffset);
    }
}
static void successfulInit() {
    RadeonNIAccel a; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(status == 0x1234abcd && a.fUvdBytes && a.fUvdState == RDN_UVD_DIAGNOSTIC_RING_READY);
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(calls.init == 1 && calls.alloc == 1 && !calls.freed);
    assert(calls.firmwareAlloc == calls.firmwareFree);
    assert(a.provider.marker == kOSBooleanTrue && !a.provider.removes);
    a.prepareToRetire();
    assert(a.provider.marker == kOSBooleanTrue && a.fUvdBytes && !calls.freed);
}
static void quarantine() {
    RadeonNIAccel a; calls.startedFailure = true; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnError);
    assert(a.fUvdState == RDN_UVD_DIAGNOSTIC_FAULTED && a.fUvdBytes && a.fUvdOffset);
    assert(!calls.freed && a.fUvdError == -5);
    calls.startedFailure = false;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    assert(calls.init == 1 && calls.alloc == 1 && !calls.freed);
    a.prepareToRetire();
    assert(a.provider.marker == kOSBooleanTrue && !a.provider.removes);
}
static void retryBeforeHardware() {
    RadeonNIAccel a; calls.prehardwareFailure = true; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnError);
    assert(!a.fUvdState && !a.fUvdBytes && !a.fUvdOffset && calls.freed == 1);
    assert(!a.fUvd.failed && !a.fUvd.started);
    assert(!a.provider.marker && a.provider.removes == 1);
    calls.prehardwareFailure = false;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(calls.init == 2 && calls.alloc == 2 && calls.freed == 1);
    assert(a.provider.marker == kOSBooleanTrue && a.provider.sets == 2);
}

static void markerFailure() {
    for (bool failSet : {false, true}) {
        RadeonNIAccel a; a.provider.failSet = failSet; a.provider.dropSet = !failSet;
        UInt32 status; int before = calls.freed;
        assert(a.uvdDiagnosticInit(&status) == kIOReturnNoMemory && !status);
        assert(calls.freed == before + 1 && calls.freedOffset == calls.offset);
        assert(!calls.init && !a.fUvdBytes && !a.fUvdOffset && !a.provider.marker);
        assert(calls.firmwareAlloc == calls.firmwareFree);
    }
}
static void priorMarker() {
    RadeonNIAccel a; a.provider.marker = kOSBooleanTrue; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady && !status);
    requireNoDeviceWork(); assert(!calls.firmwareAlloc && !a.provider.sets);
    assert(a.provider.marker == kOSBooleanTrue && !a.provider.removes);
}
static void retirementGuard() {
    RadeonNIAccel a; a.prepareToRetire(); a.prepareToRetire();
    a.fFramebuffer = nullptr; a.fUvdProvider = nullptr; UInt32 status = 99;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady && !status);
    requireNoDeviceWork(); assert(!calls.firmwareAlloc);
}
static void concurrentRetire() {
    RadeonNIAccel a; holdInit = true;
    UInt32 status = 0; int result = -1; std::atomic<bool> retired{false};
    std::thread init([&] { result = a.uvdDiagnosticInit(&status); });
    {
        std::unique_lock<std::mutex> guard(gateMutex);
        gate.wait(guard, [] { return initEntered; });
    }
    std::thread retire([&] { a.prepareToRetire(); retired = true; });
    {
        std::unique_lock<std::mutex> guard(gateMutex);
        gate.wait(guard, [] { return lockAttempts.load() == 2; });
        assert(!retired.load()); releaseInit = true;
    }
    gate.notify_all(); init.join(); retire.join();
    assert(result == kIOReturnSuccess && retired.load() && a.fRetiring);
    assert(a.provider.marker == kOSBooleanTrue && a.fUvdBytes && !calls.freed);
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady && !status);
    assert(calls.init == 1 && calls.alloc == 1);
}

static void concurrentInit() {
    RadeonNIAccel a; holdInit = true;
    UInt32 firstStatus = 0, secondStatus = 0; int first = -1, second = -1;
    std::thread one([&] { first = a.uvdDiagnosticInit(&firstStatus); });
    {
        std::unique_lock<std::mutex> guard(gateMutex);
        gate.wait(guard, [] { return initEntered; });
    }
    std::thread two([&] { second = a.uvdDiagnosticInit(&secondStatus); });
    {
        std::unique_lock<std::mutex> guard(gateMutex);
        gate.wait(guard, [] { return lockAttempts.load() == 2; });
        releaseInit = true;
    }
    gate.notify_all(); one.join(); two.join();
    assert(first == kIOReturnSuccess && second == kIOReturnSuccess);
    assert(firstStatus == secondStatus && firstStatus == 0x1234abcd);
    assert(calls.init == 1 && calls.alloc == 1 && calls.firmwareAlloc == 2);
}
static void query() {
    RadeonNIAccel a; a.fUvdStatus = 77; a.fUvdError = -5; a.fUvdBytes = 2048;
    rdn_user_uvd_diagnostic info; memset(&info, 0xff, sizeof(info));
    a.uvdDiagnosticInfo(&info);
    assert(info.version == RDN_UVD_DIAGNOSTIC_VERSION);
    assert(info.flags == (RDN_UVD_DIAGNOSTIC_ALLOWED | RDN_UVD_DIAGNOSTIC_FIRMWARE | RDN_UVD_DIAGNOSTIC_RLC_FIRMWARE));
    assert(info.last_status == 77 && info.last_error == -5 && info.reserved_bytes == 2048);
    assert(!info.reserved[0] && !info.reserved[1]);
    requireNoDeviceWork(); assert(!calls.firmwareAlloc);
}
static void retiredQuery() {
    RadeonNIAccel a; a.fUvdState = RDN_UVD_DIAGNOSTIC_FAULTED;
    a.fUvdStatus = 77; a.fUvdError = -5; a.fUvdBytes = 2048;
    a.prepareToRetire(); a.fFramebuffer = nullptr; a.fUvdProvider = nullptr;
    rdn_user_uvd_diagnostic info; memset(&info, 0xff, sizeof(info));
    a.uvdDiagnosticInfo(&info);
    assert(info.version == RDN_UVD_DIAGNOSTIC_VERSION && !info.flags);
    assert(info.state == RDN_UVD_DIAGNOSTIC_FAULTED && info.last_status == 77);
    assert(info.last_error == -5 && info.reserved_bytes == 2048);
    assert(!info.reserved[0] && !info.reserved[1]);
    requireNoDeviceWork(); assert(!calls.firmwareAlloc);
}
static void userGuards() {
    RadeonNIAccel a; RadeonNIUserClient c; c.fAccel = &a; UInt32 status = 99;
    assert(c.methodUvdDiagnosticInit(0, nullptr) == kIOReturnBadArgument);
    assert(c.methodUvdDiagnosticInit(1, &status) == kIOReturnBadArgument);
    assert(c.methodUvdDiagnosticInit(0, &status) == kIOReturnNotPrivileged && !status);
    c.fAccel = nullptr; c.fUvdAdministrator = true;
    assert(c.methodUvdDiagnosticInit(0, &status) == kIOReturnBadArgument);
    c.fAccel = &a;
    rdn_user_uvd_diagnostic info; IOByteCount size = sizeof(info) - 1;
    assert(c.methodUvdDiagnosticInfo(&info, &size) == kIOReturnBadArgument);
    assert(c.methodUvdDiagnosticInfo(nullptr, &size) == kIOReturnBadArgument);
    assert(c.methodUvdDiagnosticInfo(&info, nullptr) == kIOReturnBadArgument);
    requireNoDeviceWork(); assert(!calls.firmwareAlloc);
    size = sizeof(info); assert(c.methodUvdDiagnosticInfo(&info, &size) == kIOReturnSuccess);
    assert(size == sizeof(info));
    assert(c.methodUvdDiagnosticInit(0, &status) == kIOReturnSuccess && calls.init == 1);
}
static void coldPrepared() {
    RadeonNIAccel a; a.fEngineUp = false;
    RadeonNIAccel::prepareUvdBeforeCP(&a);
    assert(a.fUvdColdAttempted && a.fUvd.prepared && a.fUvd.started);
    assert(a.fUvdState == RDN_UVD_DIAGNOSTIC_OFF && a.fUvdBytes && a.fUvdOffset);
    assert(calls.prepare == 1 && !calls.init && !calls.start);
    assert(!calls.emit && !calls.done && !calls.sleeps && !calls.flush);
    assert(calls.alloc == 1 && !calls.freed && a.provider.marker == kOSBooleanTrue);
    assert(calls.firmwareAlloc == 2 && calls.firmwareFree == 2);
    // Preparation owns its copy; a later property change cannot replace it.
    a.framebuffer.firmwarePresent = a.framebuffer.rlcPresent = false;
    a.fEngineUp = true; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(status == 0x1234abcd && a.fUvdState == RDN_UVD_DIAGNOSTIC_RING_READY);
    assert(calls.start == 1 && calls.prepare == 1 && !calls.init);
    assert(calls.emit == 1 && calls.flush == 1 && calls.alloc == 1 && !calls.freed);
    assert(calls.firmwareAlloc == 2 && a.provider.sets == 1 && !a.provider.removes);
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess && calls.start == 1);
}
static void coldFailure(bool touchedHardware) {
    RadeonNIAccel a; a.fEngineUp = false;
    calls.startedFailure = touchedHardware; calls.prehardwareFailure = !touchedHardware;
    RadeonNIAccel::prepareUvdBeforeCP(&a);
    assert(a.fUvdColdAttempted && !a.fUvd.prepared && calls.prepare == 1);
    assert(!calls.emit && !calls.init && !calls.start);
    assert(bool(a.fUvdBytes) == touchedHardware && bool(a.provider.marker) == touchedHardware);
    assert(calls.freed == (touchedHardware ? 0 : 1));
    a.fEngineUp = true; calls.startedFailure = calls.prehardwareFailure = false;
    UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    assert(calls.alloc == 1 && calls.prepare == 1 && !calls.init && !calls.start && !calls.emit);
}
static void coldStartFailure() {
    RadeonNIAccel a; a.fEngineUp = false; RadeonNIAccel::prepareUvdBeforeCP(&a);
    a.fEngineUp = true; calls.startedFailure = true; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnError);
    assert(a.fUvdState == RDN_UVD_DIAGNOSTIC_FAULTED && a.fUvdBytes && a.fUvd.started);
    assert(!calls.freed && a.provider.marker == kOSBooleanTrue && !a.provider.removes);
    calls.startedFailure = false;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    assert(calls.prepare == 1 && calls.start == 1 && calls.alloc == 1 && calls.emit == 1);
}
static void coldMarkerLoss() {
    RadeonNIAccel a; a.fEngineUp = false; RadeonNIAccel::prepareUvdBeforeCP(&a);
    a.fEngineUp = true; UInt32 status; OSObject replacement;
    a.provider.marker = nullptr;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    a.provider.marker = &replacement;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    assert(a.fUvd.prepared && a.fUvdBytes && calls.prepare == 1 && !calls.start);
    assert(!calls.emit && !calls.freed && !a.provider.removes);
}
static void coldCpFailure() {
    RadeonNIAccel a; a.fEngineUp = false; RadeonNIAccel::prepareUvdBeforeCP(&a);
    // A failed CP startup must not grant the later administrative start.
    UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    assert(!calls.emit && !calls.start && !calls.freed && a.provider.marker);
    a.prepareToRetire(); a.fFramebuffer = nullptr; a.fUvdProvider = nullptr;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    assert(!calls.emit && !calls.start && !calls.freed);
}
static void coldFenceTimeout() {
    RadeonNIAccel a; a.fEngineUp = false; RadeonNIAccel::prepareUvdBeforeCP(&a);
    a.fEngineUp = true; calls.completeAfterSleeps = 1001; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnTimeout);
    assert(a.fUvd.prepared && a.fUvdBytes && a.provider.marker && !calls.freed && !calls.start);
    assert(calls.sleeps == 1000 && calls.prepare == 1 && calls.alloc == 1);
    calls.completeAfterSleeps = 0;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnSuccess);
    assert(calls.emit == 2 && calls.start == 1 && calls.prepare == 1 && calls.alloc == 1);
}
static void coldPreflightFailure() {
    RadeonNIAccel a; a.fEngineUp = false; a.framebuffer.rlcPresent = false;
    RadeonNIAccel::prepareUvdBeforeCP(&a);
    assert(a.fUvdColdAttempted && !a.fUvd.prepared && !calls.prepare && !calls.alloc);
    a.framebuffer.rlcPresent = true; a.fEngineUp = true; UInt32 status;
    assert(a.uvdDiagnosticInit(&status) == kIOReturnNotReady);
    assert(!calls.emit && !calls.prepare && !calls.start && !calls.init && !calls.alloc);
}
int main(int argc, char **argv) {
    assert(argc == 2); std::string test(argv[1]);
    if (test == "disabled") disabled();
    else if (test == "missing_firmware") missingFirmware();
    else if (test == "invalid_firmware") invalidFirmware();
    else if (test == "missing_rlc") missingRlc();
    else if (test == "malformed_rlc") malformedRlc();
    else if (test == "rlc_query") rlcQuery();
    else if (test == "emit_failure") emitFailure();
    else if (test == "fence_timeout") fenceTimeout();
    else if (test == "fence_deadline") fenceDeadline();
    else if (test == "submit_blocked") submitBlockedDuringIdle();
    else if (test == "allocation_failure") allocationFailure();
    else if (test == "allocation_range") allocationRange();
    else if (test == "success") successfulInit();
    else if (test == "quarantine") quarantine();
    else if (test == "retry_before_hardware") retryBeforeHardware();
    else if (test == "concurrent") concurrentInit();
    else if (test == "marker_failure") markerFailure();
    else if (test == "prior_marker") priorMarker();
    else if (test == "retirement") retirementGuard();
    else if (test == "concurrent_retire") concurrentRetire();
    else if (test == "query") query();
    else if (test == "retired_query") retiredQuery();
    else if (test == "user_guards") userGuards();
    else if (test == "cold_prepared") coldPrepared();
    else if (test == "cold_safe_failure") coldFailure(false);
    else if (test == "cold_hardware_failure") coldFailure(true);
    else if (test == "cold_start_failure") coldStartFailure();
    else if (test == "cold_marker_loss") coldMarkerLoss();
    else if (test == "cold_cp_failure") coldCpFailure();
    else if (test == "cold_fence_timeout") coldFenceTimeout();
    else if (test == "cold_preflight_failure") coldPreflightFailure();
    else assert(false);
}
'''
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = pathlib.Path(cls.directory.name)
        src = path / "test.cpp"
        src.write_text(harness)
        cls.exe = path / "test"
        subprocess.run(["c++", "-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "hw"), str(src), "-pthread", "-o", str(cls.exe)], check=True)

    def run_case(self, case):
        subprocess.run([str(self.exe), case], check=True, timeout=10)

    def test_disabled(self): self.run_case("disabled")
    def test_missing_firmware(self): self.run_case("missing_firmware")
    def test_missing_rlc_firmware(self): self.run_case("missing_rlc")
    def test_malformed_rlc_rejected_before_graphics(self): self.run_case("malformed_rlc")
    def test_rlc_query_requires_exact_length(self): self.run_case("rlc_query")
    def test_graphics_fence_emit_failure_allows_retry(self): self.run_case("emit_failure")
    def test_graphics_fence_timeout_is_bounded(self): self.run_case("fence_timeout")
    def test_graphics_fence_completion_at_deadline(self): self.run_case("fence_deadline")
    def test_graphics_submit_blocked_through_uvd_init(self): self.run_case("submit_blocked")
    def test_invalid_firmware(self): self.run_case("invalid_firmware")
    def test_allocation_failure(self): self.run_case("allocation_failure")
    def test_allocation_range(self): self.run_case("allocation_range")
    def test_success_is_idempotent(self): self.run_case("success")
    def test_failed_hardware_quarantines(self): self.run_case("quarantine")
    def test_prehardware_failure_allows_retry(self): self.run_case("retry_before_hardware")
    def test_concurrent_init_serializes(self): self.run_case("concurrent")
    def test_marker_creation_failure_frees_storage(self): self.run_case("marker_failure")
    def test_prior_marker_blocks_init(self): self.run_case("prior_marker")
    def test_retirement_blocks_init(self): self.run_case("retirement")
    def test_retirement_waits_for_inflight_init(self): self.run_case("concurrent_retire")
    def test_query_does_not_initialize(self): self.run_case("query")
    def test_retired_query_uses_cached_state(self): self.run_case("retired_query")
    def test_user_client_guards(self): self.run_case("user_guards")
    def test_cold_prepare_then_start_preserves_ownership(self): self.run_case("cold_prepared")
    def test_cold_prehardware_failure_cannot_fall_back(self): self.run_case("cold_safe_failure")
    def test_cold_hardware_failure_is_sticky(self): self.run_case("cold_hardware_failure")
    def test_prepared_start_failure_quarantines(self): self.run_case("cold_start_failure")
    def test_prepared_start_rejects_lost_or_replaced_marker(self): self.run_case("cold_marker_loss")
    def test_cold_cp_failure_blocks_late_start(self): self.run_case("cold_cp_failure")
    def test_prepared_fence_timeout_retains_owned_reservation(self): self.run_case("cold_fence_timeout")
    def test_cold_preflight_failure_cannot_fall_back(self): self.run_case("cold_preflight_failure")


if __name__ == "__main__":
    unittest.main()
