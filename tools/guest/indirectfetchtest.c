/* A zero-vertex indirect draw exposes CP argument fetch order without rendering. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
#define CHECK(x) do { kern_return_t e=(x);if(e){printf("FAIL line=%u error=%x\n",__LINE__,e);return 1;} }while(0)
int main(int argc,char **argv)
{
 setbuf(stdout,NULL);io_service_t s=rdn_probe_device(argc>1?argv[1]:"675d");io_connect_t c=0;
 if(!s)return 1;CHECK(IOServiceOpen(s,mach_task_self(),RDN_UC_TYPE_V2,&c));
 struct rdn_user_info info;IOByteCount size=sizeof(info);CHECK(IOConnectMethodScalarIStructureO(c,RDN_UC_GET_INFO,0,&size,&info));
 vm_address_t map=0;vm_size_t bytes=0;CHECK(IOConnectMapMemory(c,0,mach_task_self(),&map,&bytes,kIOMapAnywhere));
 unsigned ib=0,arg=0,out=0;CHECK(IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,4096,4096,&ib));CHECK(IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,4096,4096,&arg));CHECK(IOConnectMethodScalarIScalarO(c,RDN_UC_ALLOC,2,1,4096,4096,&out));
 uint64_t base=((uint64_t)info.vram_gpu_base_hi<<32)|info.vram_gpu_base_lo,source=base+arg,target=base+out;
 unsigned *p=(unsigned *)(map+arg),*cmd=(unsigned *)(map+ib);volatile unsigned *result=(volatile unsigned *)(map+out);
 for(unsigned little=0;little<2;little++) {
  memset(p,0,4096);p[1]=little?0x02000000:2;result[0]=result[1]=0;
  unsigned w[]={0xc0021100,1,(unsigned)source,(unsigned)(source>>32),0xc0012400,0,2,
   0xc0043b00,2,0x8974/4,0,(unsigned)target,(unsigned)(target>>32),
   0xc0043b00,2,0x28a88/4,0,(unsigned)(target+4),(unsigned)((target+4)>>32),0xc0002f00,1};
  memcpy(cmd,w,sizeof(w));__sync_synchronize();unsigned fence=0,done=0;
  CHECK(IOConnectMethodScalarIScalarO(c,RDN_UC_SUBMIT,2,1,ib,sizeof(w)/4,&fence));
  CHECK(IOConnectMethodScalarIScalarO(c,RDN_UC_FENCE_WAIT,2,1,fence,2000,&done));if(!done){puts("FAIL fence");return 2;}
  CHECK(IOConnectMethodScalarIScalarO(c,RDN_UC_SYNC_FOR_CPU,0,0));__sync_synchronize();
  printf("input=%s VGT8974=%08x VGT28a88=%08x\n",little?"little":"native",__builtin_bswap32(result[0]),__builtin_bswap32(result[1]));
 }
 IOServiceClose(c);IOObjectRelease(s);return 0;
}
