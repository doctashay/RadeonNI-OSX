/* Read-only PCIe and board-ROM diagnostics. */
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "rdn_user.h"
static io_connect_t conn;
static unsigned rd(unsigned offset) {
 unsigned value=0;int e=IOConnectMethodScalarIScalarO(conn,RDN_UC_REG_READ,1,1,offset,&value);
 printf("register %08x result %08x value %08x\n",offset,e,value);return e?0xffffffff:value;
}
static unsigned pci(unsigned offset){return rd(RDN_UC_REG_PCI_CONFIG|offset);}
int main(int argc,char **argv) {
 io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching(RDN_UC_SERVICE_CLASS));
 if(!service||IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE,&conn))return 1;
 io_service_t fb=IOServiceGetMatchingService(0,IOServiceMatching("RadeonNI"));
 if(fb) {
  CFTypeRef rom=IORegistryEntryCreateCFProperty(fb,CFSTR("RadeonNIVBIOS"),kCFAllocatorDefault,0);
  if(rom&&CFGetTypeID(rom)==CFDataGetTypeID()&&argc>1) {
   FILE *f=fopen(argv[1],"wb");if(!f)return 2;
   long n=CFDataGetLength(rom);if(fwrite(CFDataGetBytePtr(rom),1,n,f)!=(size_t)n)return 3;
   fclose(f);printf("ROM bytes %ld\n",n);
  }
  if(rom)CFRelease(rom);IOObjectRelease(fb);
 }
 pci(0);pci(4);pci(0x2c);
 unsigned cap=pci(0x34)&255,seen[64]={0};
 for(unsigned i=0;cap>=0x40&&cap<=0xfc&&i<48;i++) {
  cap&=~3u;if(seen[cap/4]++)break;unsigned h=pci(cap);
  if((h&255)==0x10) {
   unsigned dc=pci(cap+4),ds=pci(cap+8),lc=pci(cap+12),ls=pci(cap+16);
   printf("PCIe device_status=%04x link_speed_code=%u width=x%u link_cap=%08x slot_power_value=%u scale=%u\n",ds>>16,(ls>>16)&15,(ls>>20)&63,lc,(dc>>18)&255,(dc>>26)&3);
  }
  cap=(h>>8)&255;
 }
 unsigned ext=0x100;
 for(unsigned i=0;i<64&&ext>=0x100&&ext<=0xffc;i++) {
  unsigned h=pci(ext);if(!h||h==0xffffffff){puts("extended PCI config unavailable or no capabilities");break;}
  if((h&65535)==1){pci(ext+4);pci(ext+8);pci(ext+12);pci(ext+16);pci(ext+20);}
  unsigned next=h>>20;if(!next||next==ext)break;ext=next;
 }
 rd(0x740);rd(0x8010);rd(0xe50);
 for(int i=2;i<argc;i++)rd(strtoul(argv[i],0,0));
 IOServiceClose(conn);IOObjectRelease(service);return 0;
}
