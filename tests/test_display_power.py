"""Check display power ordering and failed-transition retries."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DisplayPowerTest(unittest.TestCase):
    def test_requests_and_retries(self):
        source = (ROOT / "kext/RadeonNI/RadeonNI.cpp").read_text()
        start = source.index("IOReturn RadeonNI::setOutputPower(")
        end = source.index("\nvoid RadeonNI::pollTimerFired", start)
        harness = r'''
#include <cassert>
using IOReturn = int;
enum { kIOReturnSuccess = 0, kIOReturnNotReady = -1, kIOReturnIOError = -2 };
#define IOLog(...) ((void)0)
static int locks, disables, enables, disable_error, enable_error;
void IOLockLock(void *) { assert(locks++ == 0); }
void IOLockUnlock(void *) { assert(--locks == 0); }
struct Card { bool crtc_on = true; };
int rdn_output_disable(Card *card, const void *, bool) {
    assert(locks == 1); ++disables; card->crtc_on = false;
    return disable_error;
}
class RadeonNI {
public:
    void *fLock = this;
    Card fCard;
    bool fDisplayRequested = true, fFramebufferRequested = true;
    bool fOutputRequested = true, fOutputOn = true, fModeSet = true;
    int fCurrentMode = 1, fCurrentDepth = 0;
    const void *modeForID(int) { return this; }
    bool useHDMI() { return false; }
    int programModeLocked(int, int) {
        assert(locks == 1); ++enables;
        if (enable_error) return kIOReturnIOError;
        fOutputOn = fCard.crtc_on = true;
        return 0;
    }
    IOReturn setOutputPower(bool, bool = false);
};
'''
        checks = r'''
int main() {
    RadeonNI fb;
    assert(!fb.setOutputPower(false));
    assert(!fb.fOutputOn && disables == 1);
    assert(!fb.setOutputPower(false, true));
    assert(!fb.setOutputPower(true));
    assert(!fb.fOutputOn && enables == 0);
    assert(!fb.setOutputPower(true, true));
    assert(fb.fOutputOn && enables == 1);
    assert(!fb.setOutputPower(true, true) && enables == 1);
    // Reverse wake order: framebuffer first, monitor last.
    assert(!fb.setOutputPower(false));
    assert(!fb.setOutputPower(false, true));
    assert(!fb.setOutputPower(true, true));
    assert(!fb.fOutputOn && enables == 1);
    enable_error = 1;
    assert(fb.setOutputPower(true) == kIOReturnIOError);
    assert(!fb.fOutputOn);
    enable_error = 0;
    assert(!fb.setOutputPower(true) && fb.fOutputOn);
    disable_error = 1;
    assert(fb.setOutputPower(false) == kIOReturnIOError);
    assert(!fb.fOutputOn && !fb.fCard.crtc_on);
    assert(!fb.setOutputPower(true) && fb.fOutputOn);
    assert(locks == 0);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder)
            (path / "test.cpp").write_text(harness + source[start:end] + checks)
            subprocess.run(["c++", "-std=c++11", str(path / "test.cpp"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
