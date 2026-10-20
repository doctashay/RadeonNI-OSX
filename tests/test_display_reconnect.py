"""Exercise the framebuffer reconnect method with injected hardware results."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReconnectTest(unittest.TestCase):
    def test_mode_failure_and_retry(self):
        source = (ROOT / "kext/RadeonNI/RadeonNI.cpp").read_text()
        start = source.index("void RadeonNI::monitorReturned()")
        end = source.index("\nIOReturn RadeonNI::registerForInterruptType", start)
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <strings.h>
#include "rdn_mode.h"
using UInt8 = uint8_t;
using UInt32 = uint32_t;
enum { RDN_EDID_MAX_SIZE = 512, kIOReturnSuccess = 0 };
#define IOLog(...) ((void)0)
static int detect_result = 128, mode_result = -1, programs;
static struct rdn_output output = {};
int rdn_output_detect(rdn_card *card, uint8_t *edid) {
    card->output = &output;
    memset(edid, 2, 128);
    return detect_result;
}
int rdn_edid_modes(const uint8_t *, int, rdn_mode *out, int, int *pref) {
    memset(out, 0, sizeof(*out));
    out[0].hdisplay = 800; out[0].vdisplay = 600;
    out[1].hdisplay = 2560; out[1].vdisplay = 1600; out[1].clock = 268000;
    *pref = 0;
    return 2;
}
bool rdn_output_mode_supported(rdn_card *, const rdn_mode *mode, bool) {
    return mode->clock <= 165000;
}
class RadeonNI {
public:
    enum { kMaxModes = 32, kDepth32 = 0 };
    rdn_card fCard = {};
    UInt8 fEdid[RDN_EDID_MAX_SIZE] = {};
    rdn_mode fModes[kMaxModes] = {};
    UInt32 fModeCount = 2, fPreferred = 2, fCurrentMode = 2;
    UInt32 fSurfaceBytes = 1920 * 1080 * 4, fCurrentDepth = 0;
    int fEdidLen = 128, properties = 0;
    bool fModeSet = true, fConnected = false;
    bool useHDMI() { return false; }
    void describeFb(const rdn_mode *mode, int, rdn_fb *fb) {
        fb->pitch_pixels = mode->hdisplay; fb->height = mode->vdisplay;
    }
    int programModeLocked(UInt32 id, UInt32) {
        ++programs;
        assert(id > 0 && id <= fModeCount);
        if (!mode_result) fCurrentMode = id;
        return mode_result;
    }
    void setProperty(const char *, const void *, int = 0) { ++properties; }
    void monitorReturned();
};
'''
        checks = r'''
int main() {
    RadeonNI fb;
    fb.fModes[1].hdisplay = 1920; fb.fModes[1].vdisplay = 1080;
    fb.monitorReturned();
    assert(!fb.fConnected && fb.fModeCount == 2 && fb.fCurrentMode == 2);
    assert(fb.fModes[1].hdisplay == 1920 && fb.fPreferred == 2);
    assert(fb.fEdid[0] == 0 && fb.properties == 0 && programs == 1);
    mode_result = 0;
    fb.monitorReturned();
    assert(fb.fConnected && fb.fModeCount == 1 && fb.fCurrentMode == 1);
    assert(fb.fPreferred == 1 && fb.fEdid[0] == 2 && programs == 2);
    fb.fConnected = false;
    detect_result = -1;
    fb.monitorReturned();
    assert(!fb.fConnected && programs == 2 && fb.fModeCount == 1);
    detect_result = 128;
    fb.monitorReturned();
    assert(fb.fConnected && programs == 3 && fb.fCurrentMode == 1);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder)
            (path / "test.cpp").write_text(harness + source[start:end] + checks)
            subprocess.run(["c++", "-std=c++11", "-I", str(ROOT / "hw"),
                            str(path / "test.cpp"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
