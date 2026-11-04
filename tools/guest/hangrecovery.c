/* Deliberately hang the CP (WAIT_REG_MEM on a value that never arrives) and check
 * that the kext's watchdog resets the GPU, completes the fence, and that a
 * normal submission runs afterwards. Usage: hangrecovery 6704 [max_seconds] */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
#define SCRATCH_REG7 0x851C
int main(int argc,char **argv) {
 io_service_t s=rdn_probe_device(argc>1?argv[1]:NULL);io_connect_t c;
 unsigned limit=argc>2?(unsigned)atoi(argv[2]):30;
 if(!s||IOServiceOpen(s,mach_task_self(),RDN_UC_TYPE_V2,&c))return 1;
 unsigned offset=0,seq=0,done=0;vm_address_t addr=0;vm_size_t bytes=0;
 if(IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,4096,4096,&offset)||IOConnectMapMemory(c,0,mach_task_self(),&addr,&bytes,kIOMapAnywhere))return 2;
 volatile unsigned *p=(volatile unsigned *)(addr+offset);
 /* PACKET3 WAIT_REG_MEM: register space, function "equal", never satisfied. */
 p[0]=0xC0053C00u; p[1]=3; p[2]=SCRATCH_REG7>>2; p[3]=0; p[4]=0xDEADBEEFu; p[5]=0xFFFFFFFFu; p[6]=10; p[7]=0x80000000u;
 for(unsigned i=8;i<16;i++)p[i]=0x80000000u;
 __asm__ volatile("sync":::"memory");
 if(IOConnectMethodScalarIScalarO(c,RDN_UC_SUBMIT,2,1,offset,8,&seq)){printf("FAIL hang submit\n");return 3;}
 IOConnectMethodScalarIScalarO(c,RDN_UC_FENCE_WAIT,2,1,seq,200,&done);
 if(done){printf("FAIL the wait did not hang (fence %u completed)\n",seq);return 4;}
 printf("hang submitted: fence %u pending\n",seq);fflush(stdout);
 unsigned waited=0;
 while(!done && waited<limit){IOConnectMethodScalarIScalarO(c,RDN_UC_FENCE_WAIT,2,1,seq,1000,&done);waited++;}
 if(!done){printf("FAIL no recovery within %u s\n",limit);return 5;}
 printf("recovered: fence %u completed after about %u s\n",seq,waited);
 /* The engine must run new work: NOP buffers through the normal path. */
 for(unsigned i=0;i<16;i++)p[i]=0x80000000u;
 __asm__ volatile("sync":::"memory");
 for(unsigned i=0;i<100;i++){
  if(IOConnectMethodScalarIScalarO(c,RDN_UC_SUBMIT,2,1,offset,16,&seq)){printf("FAIL submit after reset (%u)\n",i);return 6;}
  if(IOConnectMethodScalarIScalarO(c,RDN_UC_FENCE_WAIT,2,1,seq,500,&done)||!done){printf("FAIL fence %u after reset\n",seq);return 7;}
 }
 printf("PASS: 100 submissions completed after the reset (last fence %u)\n",seq);
 IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,offset);IOConnectUnmapMemory(c,0,mach_task_self(),addr);IOServiceClose(c);IOObjectRelease(s);return 0;
}
