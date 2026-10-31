"""Only an explicit fullscreen request may use the device's scanout fallback."""
from pathlib import Path
import subprocess,tempfile
from test_winsys_sync import extract
source=(Path(__file__).resolve().parents[1]/'mesa/target/rdn_target.c').read_text().replace('PUBLIC bool\nrdn_target_surface_screen', 'static bool rdn_target_surface_screen')
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
struct rdn_screen { uint32_t index,offset,width,height,pitch_pixels,bits_per_pixel,scanout; };
struct device { void *aperture;uint64_t aperture_size;struct rdn_screen screen;void (*screen_refresh)(struct device *);
 bool (*surface_screen)(struct device *,uint32_t,struct rdn_screen *); };
static struct device *the_device;
static char pixels[1024];static unsigned refreshes,lookups;
static void refresh(struct device *d) { refreshes++; }
static bool surface(struct device *d,uint32_t id,struct rdn_screen *s) {
 lookups++;if(id!=42)return false;*s=d->screen;s->offset=256;s->scanout=0;return true;
}
'''
cases=r'''
int main(void) {
 volatile uint32_t *p;uint32_t w,h,pitch,offset,scanout;
 struct device dev={pixels,sizeof(pixels),{0,128,16,8,16,32,0},refresh,surface};
 assert(!rdn_target_surface_screen(0,&p,&w,&h,&pitch,&offset,&scanout));
 the_device=&dev;
 assert(rdn_target_surface_screen(0,&p,&w,&h,&pitch,&offset,&scanout));
 assert(refreshes==1&&!lookups&&p==(void*)(pixels+128)&&scanout==1&&w==16&&h==8&&offset==128);
 assert(!rdn_target_surface_screen(999,&p,&w,&h,&pitch,&offset,&scanout));
 assert(refreshes==1&&lookups==1);
 assert(rdn_target_surface_screen(42,&p,&w,&h,&pitch,&offset,&scanout)&&offset==256&&!scanout);
 dev.screen.width=0;assert(!rdn_target_surface_screen(0,&p,&w,&h,&pitch,&offset,&scanout));
 puts("PASS explicit fullscreen scanout, registered surface and unknown-ID rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='rdn-fullscreen-') as d:
 p=Path(d);(p/'test.c').write_text(harness+extract(source,'rdn_target_surface_screen')+cases)
 subprocess.run(['cc','-std=gnu99','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
