/* Verify a stable alias before eviction, in RAM, and after promotion. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_accel_reg.h"
#include "rdn_probe_device.h"
#define CHECK(x) do { if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;} } while(0)
static int copy(io_connect_t c,volatile uint32_t *ib,unsigned iboff,uint64_t src,uint64_t dst,unsigned size)
{
 unsigned seq=0,done=0;
 ib[0]=PACKET3(PACKET3_CP_DMA,4);ib[1]=src;ib[2]=(src>>32)|PACKET3_CP_DMA_CP_SYNC;
 ib[3]=dst;ib[4]=dst>>32;ib[5]=size;for(unsigned i=6;i<16;i++)ib[i]=0x80000000;
 __asm__ volatile("sync":::"memory");
 return IOConnectMethodScalarIScalarO(c,RDN_UC_SUBMIT,2,1,iboff,16,&seq)||
        IOConnectMethodScalarIScalarO(c,RDN_UC_FENCE_WAIT,2,1,seq,2000,&done)||!done;
}
int main(int argc,char **argv)
{
 setbuf(stdout,0);io_service_t service=rdn_probe_device(argc>1?argv[1]:NULL);io_connect_t c;
 CHECK(service&&!IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE_V2,&c));
 struct rdn_user_info info;IOByteCount n=sizeof(info);unsigned version=0,on=0,start=0,pages=0;
 CHECK(!IOConnectMethodScalarIStructureO(c,RDN_UC_GET_INFO,0,&n,&info));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_MANAGED_INFO,0,1,&version)&&version==1);
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_GART_INFO,0,3,&on,&start,&pages)&&on);
 printf("device=%04x GART_pages=%u\n",info.pci_device_id,pages);
 vm_address_t aperture=0,shadow=0;vm_size_t length=0;unsigned source=0,dest=0,cmd=0,alias=0,replacement=0;
 const unsigned bytes=65536;
 CHECK(!IOConnectMapMemory(c,0,mach_task_self(),&aperture,&length,kIOMapAnywhere));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,bytes,4096,&source));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,bytes,4096,&dest));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,4096,4096,&cmd));
 volatile uint32_t *p=(void *)(aperture+source),*q=(void *)(aperture+dest),*ib=(void *)(aperture+cmd);
 for(unsigned i=0;i<bytes/4;i++)p[i]=0x13579bdf^i;
 __asm__ volatile("sync":::"memory");
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_MANAGED_ALIAS,3,1,source,bytes,4096,&alias));
 uint64_t base=((uint64_t)info.vram_gpu_base_hi<<32)|info.vram_gpu_base_lo,va=(uint64_t)start+alias;
 CHECK(!copy(c,ib,cmd,va,base+dest,bytes));
 for(unsigned i=0;i<bytes/4;i++)CHECK(q[i]==(0x13579bdf^i));puts("PASS VRAM alias GPU read");
 CHECK(!vm_allocate(mach_task_self(),&shadow,bytes,VM_FLAGS_ANYWHERE));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_MANAGED_EVICT,3,0,alias,(uint32_t)shadow,(uint32_t)((uint64_t)shadow>>32)));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,source));
 CHECK(!copy(c,ib,cmd,va,base+dest,bytes));
 for(unsigned i=0;i<bytes/4;i++)CHECK(q[i]==(0x13579bdf^i));puts("PASS evicted alias GPU read after VRAM free");
 for(unsigned i=0;i<bytes/4;i++)q[i]=0x2468ace0^i;
 CHECK(!copy(c,ib,cmd,base+dest,va,bytes));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,bytes,4096,&replacement));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_MANAGED_PROMOTE,2,0,alias,replacement));
 CHECK(!copy(c,ib,cmd,va,base+dest,bytes));
 for(unsigned i=0;i<bytes/4;i++)CHECK(q[i]==(0x2468ace0^i));puts("PASS RAM writes retained across promotion at unchanged VA");
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_GART_UNBIND,1,0,alias));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,replacement));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,dest));
 CHECK(!IOConnectMethodScalarIScalarO(c,RDN_UC_FREE,1,0,cmd));
 vm_deallocate(mach_task_self(),shadow,bytes);IOConnectUnmapMemory(c,0,mach_task_self(),aperture);IOServiceClose(c);IOObjectRelease(service);return 0;
}
