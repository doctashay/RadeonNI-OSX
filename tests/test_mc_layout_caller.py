"""Run the cold-layout caller against IOKit mocks, without MMIO or a device."""
import os
import pathlib
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class McLayoutCallerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "kext/RadeonNI/RadeonNI.cpp").read_text()
        accel = (ROOT / "kext/RadeonNI/RadeonNIAccel.h").read_text()
        markers = "\n".join(line for text in (source, accel)
                            for line in text.splitlines()
                            if line.startswith(("#define RDN_FIRST_START_PROPERTY ",
                                                "#define RDN_MC_BLOCKED_PROPERTY ",
                                                "#define RDN_UVD_REBOOT_PROPERTY ")))
        harness = markers + "\n" + r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <strings.h>
extern "C" {
#include "rdn_mc_layout.h"
}
using UInt8 = uint8_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using SInt64 = int64_t;
using IOOptionBits = unsigned;
using IOReturn = int;
using task_t = void *;
enum { kIOReturnSuccess, kIOReturnNotReady, kIOReturnError };
enum { kPEDisableScreen = 1, kPEReleaseScreen, kPEEnableScreen, kPEAcquireScreen };
static std::vector<std::string> events;
static void event(const std::string &s) { events.push_back(s); }
static unsigned count(const std::string &s) {
    return std::count(events.begin(), events.end(), s);
}
static size_t at(const std::string &s) {
    auto p = std::find(events.begin(), events.end(), s);
    assert(p != events.end()); return p - events.begin();
}
struct OSObject {
    virtual ~OSObject() {}
    unsigned retains = 0, releases = 0;
    virtual void retain() { ++retains; event("retain"); }
    virtual void release() { ++releases; }
} trueObject;
static OSObject *kOSBooleanTrue = &trueObject;
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
struct MetaClass { const char *getClassName() { return "mock"; } } meta;
struct IORegistryEntry : OSObject {
    std::map<std::string, OSObject *> properties;
    std::map<std::string, UInt64> numbers;
    std::string failProperty, dropProperty;
    const char *getName() { return "mock"; }
    MetaClass *getMetaClass() { return &meta; }
    OSObject *getProperty(const char *name) { return properties[name]; }
    bool setProperty(const char *name, bool value) {
        event(std::string("property:") + name);
        if (failProperty == name) return false;
        if (dropProperty != name) properties[name] = value ? kOSBooleanTrue : nullptr;
        return true;
    }
    bool setProperty(const char *name, UInt64 value, unsigned) {
        numbers[name] = value; return true;
    }
    void removeProperty(const char *name) {
        event(std::string("remove:") + name); properties.erase(name);
    }
    static IORegistryEntry *getRegistryRoot();
} registryRoot;
IORegistryEntry *IORegistryEntry::getRegistryRoot() { return &registryRoot; }
struct OSDictionary : OSObject {};
struct IOUserClient : OSObject {};
struct IOService : IORegistryEntry {
    virtual bool start(IOService *) { event("service_start"); return true; }
    virtual void stop(IOService *) { event("service_stop"); }
    virtual bool terminate(IOOptionBits) { event("terminate"); return true; }
    virtual bool requestTerminate(IOService *, IOOptionBits) { event("request_terminate"); return true; }
    virtual IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) {
        event("client4"); return kIOReturnSuccess;
    }
    virtual IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) {
        event("client5"); return kIOReturnSuccess;
    }
};
struct IOFramebuffer : IOService {
    bool start(IOService *) override { event("framebuffer_start"); return true; }
    void stop(IOService *) override { event("framebuffer_stop"); }
};
struct IOAccelerator : IOService {
    void prepareToRetire() { event("accel_prepare"); }
    void retire(IOService *) { event("accel_retire"); }
};
struct OSIterator : OSObject {
    std::vector<OSObject *> objects;
    size_t index = 0;
    bool valid = true;
    OSObject *getNextObject() { return index < objects.size() ? objects[index++] : nullptr; }
    bool isValid() { return valid; }
};
static const void *gIOServicePlane = &registryRoot;
enum { kIORegistryIterateRecursively = 1 };
enum { kIOPCIConfigCommand = 4 };
static std::vector<IORegistryEntry *> registryEntries;
static bool registryFailure = false, registryValid = true;
struct IORegistryIterator : OSObject {
    size_t index = 0;
    static IORegistryIterator *iterateOver(IORegistryEntry *, const void *, unsigned) {
        event("registry_scan");
        static IORegistryIterator iterator; iterator.index = 0;
        return registryFailure ? nullptr : &iterator;
    }
    IORegistryEntry *getNextObject() {
        return index < registryEntries.size() ? registryEntries[index++] : nullptr;
    }
    bool isValid() { return registryValid; }
};
struct IOPCIDevice : IOService {
    int depth = 0;
    bool lockFailure = false, openFailure = false, iteratorFailure = false;
    IOService *owner = nullptr;
    OSIterator clients;
    unsigned configRead16(unsigned) { return 7; }
    bool lockForArbitration(bool) {
        event("lock_attempt");
        if (lockFailure) return false;
        assert(depth == 0); ++depth; event("locked"); return true;
    }
    void unlockForArbitration() { assert(depth == 1); --depth; event("unlocked"); }
    bool isOpen() { assert(depth == 1); return owner; }
    bool open(IOService *client) {
        assert(depth == 1); event("open_attempt");
        if (openFailure || owner) return false;
        owner = client; event("opened"); return true;
    }
    void close(IOService *client) { assert(owner == client); owner = nullptr; event("closed"); }
    OSIterator *getClientIterator() {
        assert(depth == 1 && owner); clients.index = 0;
        return iteratorFailure ? nullptr : &clients;
    }
};
struct PE_Video {
    uintptr_t v_baseAddr = 0;
    UInt32 v_rowBytes = 0, v_width = 0, v_height = 0, v_depth = 0;
};
struct IOPlatformExpert {
    PE_Video video;
    IOPlatformExpert() {
        video.v_baseAddr = 0x100000; video.v_rowBytes = 7680;
        video.v_width = 1920; video.v_height = 1080; video.v_depth = 32;
    }
    bool getFailure = false;
    int failMode = 0;
    IOReturn getConsoleInfo(PE_Video *out) {
        event("console_get"); if (getFailure) return kIOReturnError;
        *out = video; return kIOReturnSuccess;
    }
    IOReturn setConsoleInfo(PE_Video *v, int mode) {
        event("console:" + std::to_string(mode));
        if (mode == failMode) return kIOReturnError;
        if (v) video = *v;
        return kIOReturnSuccess;
    }
} platform;
static bool noPlatform = false;
static IOPlatformExpert *getPlatform() { return noPlatform ? nullptr : &platform; }
struct IOMemoryMap : OSObject {
    std::vector<UInt32> words;
    size_t length;
    std::string name;
    explicit IOMemoryMap(size_t size = 4096, const char *n = "map")
        : words((size + 3) / 4, 0xa5a5a5a5), length(size), name(n) {}
    size_t getLength() { return length; }
    uintptr_t getVirtualAddress() { return reinterpret_cast<uintptr_t>(words.data()); }
    void release() override { ++releases; event("release:" + name); }
};
static void IOLog(const char *, ...) {}
static void IOFree(void *, size_t) { event("bios_free"); }
static void IOLockFree(void *) { event("lock_free"); }
extern "C" void rdn_card_fini(rdn_card *) { event("card_fini"); }
static IOMemoryMap *scratchMap = nullptr;
static unsigned syncCount = 0, corruptSync = 0;
static bool zeroReads = false;
static void OSSynchronizeIO() {
    ++syncCount; event("sync");
    if (scratchMap && syncCount == corruptSync) {
        auto *p = &scratchMap->words[RDN_MC_LAYOUT_SCRATCH_OFFSET / 4];
        if (zeroReads) memset(p, 0, 4096); else p[17] ^= 0x12345678;
    }
}
static rdn_mc_layout_result coreResult{};
static int coreReturn = 0;
static bool hardwarePosted = false;
static int firmwareHandover = 0, postReturn = 0;
extern "C" bool rdn_card_posted(rdn_card *) {
    event("posted_query"); return hardwarePosted;
}
extern "C" int rdn_handover_take(rdn_card *) {
    event("handover_take"); return firmwareHandover;
}
extern "C" int rdn_card_post(rdn_card *) {
    event("post"); hardwarePosted = true; return postReturn;
}
static IOPCIDevice *activeDevice = nullptr;
extern "C" int rdn_mc_layout_rebase_zero(rdn_card *, uint32_t flags,
    uint32_t scratch, rdn_mc_layout_result *out) {
    assert(flags == RDN_MC_LAYOUT_CONDITIONS && scratch == RDN_MC_LAYOUT_SCRATCH_OFFSET);
    assert(activeDevice && activeDevice->depth == 1 && activeDevice->owner);
    assert(activeDevice->getProperty(RDN_MC_BLOCKED_PROPERTY) == kOSBooleanTrue);
    assert(count("console:1") == 1);
    event("rebase"); *out = coreResult; return coreReturn;
}
#define super IOFramebuffer
class RadeonNI : public IOFramebuffer {
public:
    IOPCIDevice *fDevice;
    IOMemoryMap *fRegMap = nullptr, *fFbMap = nullptr;
    volatile UInt8 *fRegs = nullptr;
    void *fBios = nullptr, *fLock = nullptr;
    UInt32 fBiosSize = 0;
    bool fCardReady = false, fModeSet = false;
    IOAccelerator *fAccel = nullptr;
    bool fMCRebaseRequested = false, fMCProviderLocked = false, fMCProviderOpen = false;
    bool fMCConsoleSuspended = false, fMCRebased = false, fMCFatal = false;
    rdn_card fCard{};
    struct { UInt32 pitch_pixels = 2048, width = 1920, height = 1080; } fFb;
    explicit RadeonNI(IOPCIDevice *device) : fDevice(device) {
        device->clients.objects.push_back(this); activeDevice = device;
        registryEntries = {this};
        properties["UVDDiagnosticsRebaseVRAM"] = kOSBooleanTrue;
        properties["EnableUVDDiagnostics"] = kOSBooleanTrue;
        properties["UVDDiagnosticsBeforeCP"] = kOSBooleanTrue;
    }
    bool claimColdLayout();
    bool resumeColdConsole(bool);
    bool failedColdStart(IOService *);
    bool rebaseColdLayout(int, bool = false);
    void cleanUp();
    void stop(IOService *) override;
    bool terminate(IOOptionBits) override;
    bool requestTerminate(IOService *, IOOptionBits) override;
    IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) override;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;
    void stopHotplug() { event("hotplug_stop"); }
    bool checkScratch();
    bool bootstrap();
};
'''
        for signature in (
            "bool RadeonNI::claimColdLayout(",
            "bool RadeonNI::resumeColdConsole(",
            "bool RadeonNI::failedColdStart(",
            "bool RadeonNI::rebaseColdLayout(",
            "void RadeonNI::cleanUp(",
            "void RadeonNI::stop(",
            "bool RadeonNI::terminate(",
            "bool RadeonNI::requestTerminate(",
        ):
            harness += function(source, signature) + "\n"
        first = source.index("IOReturn RadeonNI::newUserClient(")
        for offset in (first, source.index("IOReturn RadeonNI::newUserClient(", first + 1)):
            harness += function(source[offset:], "IOReturn RadeonNI::newUserClient(") + "\n"
        scratch = block(source[source.index("bool RadeonNI::bringUp("):], "\tif (fMCRebased) {")
        harness += "bool RadeonNI::checkScratch() { UInt32 i;\n" + scratch + "\nreturn true;\n}\n"
        start = source.index("\tbool warm = rdn_card_posted", source.index("bool RadeonNI::bringUp("))
        end = source.index('\tIOLog("RadeonNI: CONFIG_MEMSIZE', start)
        harness += "bool RadeonNI::bootstrap() { int r;\n" + source[start:end] + "\nreturn true;\n}\n"
        harness += r'''
static void defaultPath() {
    IOPCIDevice d; RadeonNI a(&d); a.properties.erase("UVDDiagnosticsRebaseVRAM");
    assert(a.claimColdLayout() && !d.depth && !d.owner && !a.fMCRebaseRequested);
    assert(d.getProperty(RDN_FIRST_START_PROPERTY) == kOSBooleanTrue);
    assert(!count("opened") && !count("registry_scan") && !count("rebase"));
    assert(a.rebaseColdLayout(0) && a.claimColdLayout());
    a.properties["UVDDiagnosticsRebaseVRAM"] = kOSBooleanTrue;
    assert(!a.claimColdLayout() && !d.depth && !d.owner);
}
static void rejectedGate(unsigned which) {
    IOPCIDevice d; RadeonNI a(&d); IOService other; IOFramebuffer fb; IOAccelerator gpu;
    IOMemoryMap map;
    switch (which) {
    case 0: d.lockFailure = true; break;
    case 1: d.failProperty = RDN_FIRST_START_PROPERTY; break;
    case 2: d.dropProperty = RDN_FIRST_START_PROPERTY; break;
    case 3: d.properties[RDN_FIRST_START_PROPERTY] = &other; break;
    case 4: d.properties[RDN_MC_BLOCKED_PROPERTY] = &other; break;
    case 5: d.properties[RDN_UVD_REBOOT_PROPERTY] = &other; break;
    case 6: a.properties.erase("EnableUVDDiagnostics"); break;
    case 7: a.properties.erase("UVDDiagnosticsBeforeCP"); break;
    case 8: a.fFbMap = &map; break;
    case 9: a.fAccel = &gpu; break;
    case 10: a.fModeSet = true; break;
    case 11: d.owner = &other; break;
    case 12: d.openFailure = true; break;
    case 13: d.iteratorFailure = true; break;
    case 14: d.clients.valid = false; break;
    case 15: d.clients.objects.push_back(&other); break;
    case 16: registryFailure = true; break;
    case 17: registryValid = false; break;
    case 18: registryEntries.push_back(&fb); break;
    case 19: registryEntries.push_back(&gpu); break;
    default: assert(false);
    }
    assert(!a.claimColdLayout() && !a.fMCProviderLocked && !a.fMCProviderOpen);
    assert(!d.depth && d.owner == (which == 11 ? &other : nullptr));
    assert(count("opened") == count("closed") && !count("rebase") && !count("console:1"));
}
static void acceptedGate() {
    IOPCIDevice d; RadeonNI a(&d); IOService unrelated;
    registryEntries.push_back(&unrelated);
    assert(a.claimColdLayout() && d.depth == 1 && d.owner == &a);
    assert(a.fMCProviderLocked && a.fMCProviderOpen);
    assert(at("property:" RDN_FIRST_START_PROPERTY) < at("opened"));
    a.cleanUp(); assert(!d.depth && !d.owner && count("closed") == 1);
}
static void noHandover(unsigned which) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout()); int handover = 1;
    switch (which) {
    case 0: handover = 0; break;
    case 1: handover = -1; break;
    case 2: noPlatform = true; break;
    case 3: platform.getFailure = true; break;
    case 4: platform.video.v_baseAddr = 0; break;
    case 5: platform.video.v_rowBytes = 0; break;
    case 6: platform.video.v_width = 0; break;
    case 7: platform.video.v_height = 0; break;
    default: assert(false);
    }
    assert(!a.rebaseColdLayout(handover) && !count("rebase") && !count("console:1"));
    assert(!a.failedColdStart(&d) && !d.depth && !d.owner && !a.fMCFatal);
}
static void beforeDisable(unsigned which) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    if (which == 0) d.failProperty = RDN_MC_BLOCKED_PROPERTY;
    if (which == 1) d.dropProperty = RDN_MC_BLOCKED_PROPERTY;
    if (which == 2) platform.failMode = kPEDisableScreen;
    assert(!a.rebaseColdLayout(1) && !count("rebase"));
    assert(!a.fMCConsoleSuspended && !a.failedColdStart(&d));
    assert(!d.depth && !d.owner && !count("console:4"));
}
static void unpostedEntry(bool zeroVideo) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    if (zeroVideo) platform.video = PE_Video{};
    coreResult.access_restored = 1; coreResult.changed = 1;
    assert(a.rebaseColdLayout(0, true) && a.fMCRebased && a.fMCConsoleSuspended);
    assert(count("rebase") == 1 && !d.depth && d.owner == &a);
    IOMemoryMap map; a.fFbMap = &map; a.fModeSet = true;
    assert(a.resumeColdConsole(true));
    assert(platform.video.v_baseAddr == map.getVirtualAddress());
    assert(platform.video.v_width == 1920 && platform.video.v_height == 1080);
    assert(count("console:1") == 1 && count("console:2") == 1 && count("console:4") == 1);
}
static void postOrdering(unsigned which) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    hardwarePosted = which == 1 || which == 2;
    firmwareHandover = which == 2 ? 1 : 0;
    if (which == 3) postReturn = -5;
    if (which == 0) platform.video = PE_Video{};
    coreResult.access_restored = 1; coreResult.changed = 1;
    assert(a.bootstrap() == (which == 0 || which == 2));
    assert(at("posted_query") < at("post") && at("handover_take") < at("post"));
    if (which == 0 || which == 2) {
        assert(at("post") < at("console:1") && at("post") < at("rebase"));
        assert(bool(a.getProperty("MCLayoutUnpostedEntry")) == (which == 0));
    } else assert(!count("rebase") && !count("console:1"));
}
static void safeRollback(bool preflight) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    coreReturn = -5; coreResult.old_base = 0xf00000000ull;
    coreResult.access_restored = 1; coreResult.rollback_ok = !preflight;
    coreResult.modeset_required = !preflight;
    assert(!a.rebaseColdLayout(1) && !a.fMCConsoleSuspended);
    assert(count("rebase") == 1 && count("console:3") == 1 && count("console:4") == 1);
    assert(!d.getProperty(RDN_MC_BLOCKED_PROPERTY));
    assert(a.numbers["MCLayoutBase"] == coreResult.old_base);
    assert(!a.failedColdStart(&d) && !d.owner && !d.depth && !a.retains);
}
static void quarantined(unsigned which) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    IOMemoryMap map(4096, "fb"), regs(4096, "regs"); a.fFbMap = &map; a.fRegMap = &regs;
    coreReturn = which == 2 ? 0 : -5;
    coreResult.access_restored = which == 1 ? 1 : 0;
    coreResult.modeset_required = 1;
    coreResult.changed = which == 0 ? 0 : 1;
    assert(!a.rebaseColdLayout(1) && a.fMCConsoleSuspended && !count("console:3"));
    assert(a.failedColdStart(&d) && a.fMCFatal && a.retains == 1);
    assert(d.owner == &a && !d.depth && a.fFbMap == &map && !map.releases && !regs.releases);
    assert(d.getProperty(RDN_MC_BLOCKED_PROPERTY) == kOSBooleanTrue);
    assert(count("service_start") == 1 && !count("framebuffer_start"));
    const auto before = events.size();
    a.stop(&d); a.cleanUp(); assert(!a.terminate(0) && !a.requestTerminate(&d, 0));
    IOUserClient *client = reinterpret_cast<IOUserClient *>(1);
    assert(a.newUserClient(nullptr, nullptr, 0, &client) == kIOReturnNotReady && !client);
    client = reinterpret_cast<IOUserClient *>(1);
    assert(a.newUserClient(nullptr, nullptr, 0, nullptr, &client) == kIOReturnNotReady && !client);
    assert(a.newUserClient(nullptr, nullptr, 0, nullptr) == kIOReturnNotReady);
    assert(a.newUserClient(nullptr, nullptr, 0, nullptr, nullptr) == kIOReturnNotReady);
    assert(events.size() == before && d.owner == &a && !map.releases && !regs.releases);
}
static void successfulRebase(bool lateFailure) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    coreResult.access_restored = 1; coreResult.changed = 1; coreResult.modeset_required = 1;
    assert(a.rebaseColdLayout(1) && a.fMCRebased && a.fMCConsoleSuspended && !d.depth);
    assert(d.owner == &a && !count("console:3") && at("console:1") < at("rebase"));
    IOMemoryMap map(4096, "fb"); a.fFbMap = &map; a.fModeSet = true;
    assert(a.resumeColdConsole(true) && !a.fMCConsoleSuspended);
    assert(platform.video.v_baseAddr == map.getVirtualAddress());
    assert(platform.video.v_width == 1920 && platform.video.v_height == 1080);
    assert(platform.video.v_rowBytes == 8192 && platform.video.v_depth == 32);
    assert(at("console:2") < at("console:3") && at("console:3") < at("console:4"));
    assert(d.getProperty(RDN_MC_BLOCKED_PROPERTY) == kOSBooleanTrue);
    if (lateFailure) {
        assert(a.failedColdStart(&d) && a.fMCFatal && a.fMCConsoleSuspended);
        assert(count("console:1") == 2 && !map.releases && d.owner == &a);
    } else {
        a.cleanUp();
        assert(a.fMCConsoleSuspended && !d.owner && map.releases == 1);
        assert(at("release:fb") > at("console:4") && count("console:1") == 2);
    }
}
static void failedResume(unsigned which) {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    coreResult.access_restored = 1; coreResult.changed = 1;
    assert(a.rebaseColdLayout(1));
    IOMemoryMap map; a.fFbMap = &map; a.fModeSet = true;
    if (which == 0) a.fModeSet = false;
    if (which == 1) a.fFbMap = nullptr;
    if (which == 2) platform.getFailure = true;
    if (which >= 3) platform.failMode = which - 1;
    assert(!a.resumeColdConsole(true) && a.fMCConsoleSuspended);
    assert(d.getProperty(RDN_MC_BLOCKED_PROPERTY));
    assert(a.failedColdStart(&d) && a.fMCFatal && d.owner == &a);
}
static void cleanupResources() {
    IOPCIDevice d; RadeonNI a(&d); assert(a.claimColdLayout());
    IOMemoryMap map(4096, "fb"), regs(4096, "regs");
    a.fFbMap = &map; a.fRegMap = &regs; a.fRegs = reinterpret_cast<UInt8 *>(1);
    a.fCardReady = true; a.fBios = reinterpret_cast<void *>(1); a.fLock = a.fBios;
    a.cleanUp(); a.cleanUp();
    assert(!a.fCardReady && !a.fBios && !a.fLock && !a.fFbMap && !a.fRegMap && !a.fRegs);
    assert(!d.owner && !d.depth && count("closed") == 1 && count("unlocked") == 1);
    assert(count("card_fini") == 1 && count("bios_free") == 1 && count("lock_free") == 1);
    assert(map.releases == 1 && regs.releases == 1);
}
static void scratchCheck(unsigned which) {
    IOPCIDevice d; RadeonNI a(&d); a.fMCRebased = true;
    IOMemoryMap map(RDN_MC_LAYOUT_SCRATCH_OFFSET + 8192); a.fFbMap = &map; scratchMap = &map;
    if (which == 1) map.length = RDN_MC_LAYOUT_SCRATCH_OFFSET + 4095;
    if (which == 2 || which == 3) { corruptSync = 1; zeroReads = which == 3; }
    if (which == 4) corruptSync = 2;
    if (which == 5) a.failProperty = "MCLayoutScratchVerified";
    if (which == 6) a.dropProperty = "MCLayoutScratchVerified";
    assert(a.checkScratch() == (which == 0));
    assert((a.getProperty("MCLayoutScratchVerified") == kOSBooleanTrue) == (which == 0));
    size_t first = RDN_MC_LAYOUT_SCRATCH_OFFSET / 4, last = first + 1024;
    assert(map.words[first - 1] == 0xa5a5a5a5 && map.words[last] == 0xa5a5a5a5);
    if (which == 0) for (size_t i = first; i < last; ++i) assert(!map.words[i]);
    if (which == 1) assert(!syncCount && map.words[first] == 0xa5a5a5a5);
}
static void ordinaryDispatch() {
    IOPCIDevice d; RadeonNI a(&d); IOUserClient *client = nullptr;
    assert(a.terminate(0) && a.requestTerminate(&d, 0));
    assert(a.newUserClient(nullptr, nullptr, 0, &client) == kIOReturnSuccess);
    assert(a.newUserClient(nullptr, nullptr, 0, nullptr, &client) == kIOReturnSuccess);
    assert(count("terminate") == 1 && count("request_terminate") == 1);
    assert(count("client4") == 1 && count("client5") == 1);
}
int main(int argc, char **argv) {
    assert(argc >= 2); std::string name = argv[1];
    unsigned n = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 0;
    if (name == "default") defaultPath();
    else if (name == "reject") rejectedGate(n);
    else if (name == "accept") acceptedGate();
    else if (name == "handover") noHandover(n);
    else if (name == "before_disable") beforeDisable(n);
    else if (name == "unposted") unpostedEntry(n);
    else if (name == "post_order") postOrdering(n);
    else if (name == "safe") safeRollback(n);
    else if (name == "fatal") quarantined(n);
    else if (name == "success") successfulRebase(n);
    else if (name == "resume_failure") failedResume(n);
    else if (name == "cleanup") cleanupResources();
    else if (name == "scratch") scratchCheck(n);
    else if (name == "dispatch") ordinaryDispatch();
    else assert(false);
}
'''
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        path = pathlib.Path(cls.directory.name)
        src = path / "test.cpp"
        src.write_text(harness)
        cls.exe = path / "test"
        cls.runner = shlex.split(os.environ.get("MC_CALLER_RUNNER", ""))
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        flags = shlex.split(os.environ.get("MC_CALLER_CXXFLAGS", "-O2"))
        subprocess.run(compiler + ["-std=c++11", "-Wall", "-Wextra", "-Werror"] + flags +
                       ["-I", str(ROOT / "hw"), str(src), "-o", str(cls.exe)], check=True)

    def run_case(self, case, variants=1):
        for number in range(variants):
            with self.subTest(case=case, variant=number):
                subprocess.run(self.runner + [str(self.exe), case, str(number)],
                               check=True, timeout=15)

    def test_default_path_marks_first_start_without_device_work(self): self.run_case("default")
    def test_gate_rejections_release_only_owned_resources(self): self.run_case("reject", 20)
    def test_empty_graphics_registry_accepts_and_holds_provider(self): self.run_case("accept")
    def test_fresh_handover_and_console_are_required(self): self.run_case("handover", 8)
    def test_marker_and_disable_failures_do_not_rebase(self): self.run_case("before_disable", 3)
    def test_strict_unposted_entry_accepts_zero_boot_video(self): self.run_case("unposted", 2)
    def test_cold_evidence_is_captured_before_post_and_rebase_follows_post(self): self.run_case("post_order", 4)
    def test_safe_rollback_resumes_old_console(self): self.run_case("safe", 2)
    def test_unsafe_rollback_quarantines_and_rejects_clients(self): self.run_case("fatal", 3)
    def test_success_defers_console_until_modeset_and_pins_late_failure(self): self.run_case("success", 2)
    def test_incomplete_console_restore_keeps_quarantine(self): self.run_case("resume_failure", 6)
    def test_cleanup_is_balanced_and_idempotent(self): self.run_case("cleanup")
    def test_real_scratch_pattern_zeroing_and_guard_bytes(self): self.run_case("scratch", 7)
    def test_nonfatal_termination_and_client_dispatch_stay_unchanged(self): self.run_case("dispatch")


if __name__ == "__main__":
    unittest.main()
