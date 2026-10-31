/* Reject malformed detailed timings without losing valid 1080p60. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../hw/rdn_mode.h"
int main(void)
{
 unsigned char edid[128] = {0};
 const unsigned char valid[18] = {0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,0x58,0x2c,0x45,0,0xc4,0x8e,0x21,0,0,0x1e};
 struct rdn_mode mode;
 unsigned checks = 0;
 memcpy(edid + 54, valid, 18);
 assert(rdn_edid_detailed_mode(edid,0,&mode));checks++;
 assert(mode.hdisplay==1920 && mode.vdisplay==1080 && mode.clock==148500);checks++;
 for(unsigned field=0;field<5;field++) {
  unsigned char *d=edid+54;memcpy(d,valid,18);
  switch(field) {
  case 0:d[2]=0;d[4]&=15;break;
  case 1:d[5]=0;d[7]&=15;break;
  case 2:d[9]=0;d[11]&=0xcf;break;
  case 3:d[8]=255;d[11]|=0xc0;break;
  case 4:d[10]=0xff;d[11]|=15;break;
  }
  assert(!rdn_edid_detailed_mode(edid,0,&mode));checks++;
 }
 puts("PASS: detailed timing validation (7 checks)");
 return checks!=7;
}
