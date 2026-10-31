"""Run the actual IRQ drain action against a simulated IH ring."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'kext/RadeonNI/RadeonNIFence.cpp').read_text()
a = s.index('void RadeonNIAccel::fenceInterruptAction(')
b = s.index('\nIOReturn RadeonNIAccel::startFenceInterrupts', a)
action = s[a:b].replace('__asm__ __volatile__("sync" : : : "memory");', '/* Host memory is coherent in this fixture. */')
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#define IH_BYTES 65536u
#define IH_RB_WPTR 0x3e0c
#define IH_RB_RPTR 0x3e08
#define IH_RB_CNTL 0x3e00
using UInt32=uint32_t;
struct OSObject {};
struct IOInterruptEventSource {};
struct rdn_card { unsigned wptr=0,control=0,published=0,arrivals=0,writes=0; };
struct accel { rdn_card *card; };
struct RadeonNIAccel:OSObject {
 bool fFenceIRQEnabled=true,overflow=false;unsigned fFenceRptr=0,fFenceRingOffset=0,fFenceEvents=0;int fLock=0;accel fAccel;
 static void fenceInterruptAction(OSObject *,IOInterruptEventSource *,int);
 void setProperty(const char *,bool v){overflow=v;}
};
static unsigned wakes;
static void IOLockLock(int lock) {}
static void IOLockUnlock(int lock) {}
static void IOLockWakeup(int lock,unsigned *p,bool one){wakes++;}
static unsigned rdn_rreg(rdn_card *c,unsigned reg){return reg==IH_RB_WPTR?c->wptr:c->control;}
static void rdn_wreg(rdn_card *c,unsigned reg,unsigned value) {
 if(reg==IH_RB_RPTR){c->published=value;c->writes++;if(c->arrivals){c->arrivals--;c->wptr=(c->wptr+16)&(IH_BYTES-1);}}
 else {c->control=value;c->wptr&=~1u;}
}
static unsigned rdn_vram_read32(accel *a,unsigned offset){assert(offset<IH_BYTES&&!(offset&15));return 176;}
'''
cases = r'''
int main() {
 rdn_card card;RadeonNIAccel driver;driver.fAccel.card=&card;
 card.wptr=16;card.arrivals=2;
 driver.fenceInterruptAction(&driver,nullptr,1);
 assert(driver.fFenceEvents==3&&driver.fFenceRptr==48&&card.published==48&&wakes==1);
 driver.fFenceRptr=IH_BYTES-16;card.wptr=16;card.arrivals=0;
 driver.fenceInterruptAction(&driver,nullptr,1);
 assert(driver.fFenceEvents==5&&driver.fFenceRptr==16);
 driver.fFenceRptr=0;card.wptr=49;card.writes=0;
 driver.fenceInterruptAction(&driver,nullptr,1);
 assert(driver.overflow&&driver.fFenceRptr==48&&card.writes==4095);
 driver.fFenceIRQEnabled=false;unsigned events=driver.fFenceEvents;card.wptr=64;
 driver.fenceInterruptAction(&driver,nullptr,1);assert(driver.fFenceEvents==events);
 puts("PASS IRQ arrivals during drain, wraparound, overflow recovery and disabled handler");
}
'''
with tempfile.TemporaryDirectory(prefix='rdn-irq-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(prefix+action+cases)
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++11','-Wall','-Werror']+shlex.split(os.environ.get('CXXFLAGS',''))+['-o',str(path/'test'),str(path/'test.cpp')],check=True)
    subprocess.run(shlex.split(os.environ.get('TEST_RUNNER',''))+[str(path/'test')],check=True)
