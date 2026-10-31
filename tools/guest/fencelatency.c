/* Bounded no-op submissions verify fence progress and interrupt delivery. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
int main(int argc,char **argv) {
 io_service_t s=rdn_probe_device(argc>1?argv[1]:NULL);io_connect_t c;
 if(!s||IOServiceOpen(s,mach_task_self(),RDN_UC_TYPE_V2,&c))return 1;
 unsigned offset=0,seq=0,done=0;vm_address_t addr=0;vm_size_t bytes=0;
 if(IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,4096,4096,&offset)||IOConnectMapMemory(c,0,mach_task_self(),&addr,&bytes,kIOMapAnywhere))return 2;
 volatile unsigned *p=(volatile unsigned *)(addr+offset);for(unsigned i=0;i<16;i++)p[i]=0x80000000u;
 __asm__ volatile("sync":::"memory");mach_timebase_info_data_t tb;mach_timebase_info(&tb);unsigned long long start=mach_absolute_time();
 for(unsigned i=0;i<1000;i++) {
  kern_return_t error=IOConnectMethodScalarIScalarO(c,RDN_UC_SUBMIT,2,1,offset,16,&seq);
  if(error){printf("FAIL submit error=0x%08x iteration=%u\n",error,i);return 3;}
  if(IOConnectMethodScalarIScalarO(c,RDN_UC_FENCE_WAIT,2,1,seq,100,&done)||!done){printf("FAIL fence %u\n",seq);return 4;}
 }
 double ms=(mach_absolute_time()-start)*(double)tb.numer/tb.denom/1e6;
 printf("PASS: 1000 fences, last=%u elapsed_ms=%.3f\n",seq,ms);
 IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,offset);IOConnectUnmapMemory(c,0,mach_task_self(),addr);IOServiceClose(c);IOObjectRelease(s);return 0;
}
