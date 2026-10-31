/* Verify command submission while the ordinary visible heap is full. */
#include <stdio.h>
#include <stdint.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
int main(int argc,char **argv)
{
 io_service_t service=rdn_probe_device(argc>1?argv[1]:NULL);io_connect_t c=0;
 unsigned held[4096],count=0,cmd=0,version=0,seq=0,done=0;int result=1;
 vm_address_t aperture=0;vm_size_t length=0;
 if(!service||IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE_V2,&c))return 2;
 if(IOConnectMethodScalarIScalarO(c,RDN_UC_COMMAND_INFO,0,1,&version)||version!=1)goto cleanup;
 if(IOConnectMapMemory(c,0,mach_task_self(),&aperture,&length,kIOMapAnywhere))goto cleanup;
 for(unsigned size=4u<<20;size>=65536;size/=64) {
  unsigned at;
  while(count<4096&&!IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,size,4096,&at))held[count++]=at;
 }
 unsigned normal=0;int ordinary=IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,65536,4096,&normal);
 if(!ordinary){IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,normal);goto cleanup;}
 if(IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC_COMMAND,2,1,65536,4096,&cmd))goto cleanup;
 volatile uint32_t *words=(void*)(aperture+cmd);for(unsigned i=0;i<16;i++)words[i]=0x80000000;
 __asm__ volatile("sync":::"memory");
 if(IOConnectMethodScalarIScalarO(c,RDN_UC_SUBMIT,2,1,cmd,16,&seq)||
    IOConnectMethodScalarIScalarO(c,RDN_UC_FENCE_WAIT,2,1,seq,2000,&done)||!done)goto cleanup;
 result=0;printf("PASS device=%s ordinary=%08x reserved_command=%08x fence=%u complete=%u allocations=%u\n",argv[1],ordinary,cmd,seq,done,count);
cleanup:
 if(cmd)IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,cmd);
 for(unsigned i=0;i<count;i++)IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,held[i]);
 if(aperture)IOConnectUnmapMemory(c,0,mach_task_self(),aperture);
 IOServiceClose(c);IOObjectRelease(service);if(result)puts("FAIL reserved command pool");return result;
}
