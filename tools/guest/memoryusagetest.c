/* Check device-wide accounting through a second user client. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
static uint64_t value(struct rdn_user_bytes b) { return ((uint64_t)b.hi<<32)|b.lo; }
static int sample(io_connect_t c, const char *label, struct rdn_user_memory_usage *m)
{
 size_t size=sizeof(*m);
 kern_return_t e=IOConnectCallStructMethod(c,RDN_UC_MEMORY_USAGE,NULL,0,m,&size);
 if(e || size!=sizeof(*m) || m->version!=1 || m->size!=sizeof(*m)) {
  fprintf(stderr,"%s query failed: %08x\n",label,e);return 1;
 }
 uint64_t total=value(m->total),reserved=value(m->reserved),allocated=value(m->allocated),freeBytes=value(m->free);
 printf("%s total=%llu reserved=%llu allocated=%llu free=%llu\n",label,total,reserved,allocated,freeBytes);
 return total!=reserved+allocated+freeBytes || freeBytes!=value(m->visible_free)+value(m->hidden_free)+value(m->command_free) ||
  value(m->visible_free)>value(m->visible_capacity) || value(m->hidden_free)>value(m->hidden_capacity) ||
  value(m->command_free)>value(m->command_capacity) || value(m->visible_largest)>value(m->visible_free) || value(m->hidden_largest)>value(m->hidden_free);
}
static kern_return_t allocate(io_connect_t c, unsigned method, unsigned bytes, uint32_t *offset)
{
 uint64_t input[2]={bytes,4096},output=0;uint32_t count=1;
 kern_return_t e=IOConnectCallScalarMethod(c,method,input,2,&output,&count);
 if(!e && count==1 && output<=0xffffffffu)*offset=(uint32_t)output;
 else if(!e)e=kIOReturnBadArgument;
 return e;
}
int main(int argc,char **argv)
{
 if(argc!=2)return 2;
 io_service_t service=rdn_probe_device(argv[1]);io_connect_t owner=0,observer=0;
 uint32_t visible=0,hidden=0;int haveHidden=0,failed=1;
 struct rdn_user_memory_usage before,during,after;
 if(!service)return 1;
 if(IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE_V2,&owner) || IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE_V2,&observer))goto out;
 if(sample(observer,"before",&before))goto out;
 if(allocate(owner,RDN_UC_ALLOC,4u<<20,&visible))goto out;
 if(value(before.hidden_capacity)) {
  if(allocate(owner,RDN_UC_ALLOC_HIDDEN,16u<<20,&hidden))goto out;haveHidden=1;
 }
 if(sample(observer,"allocated",&during))goto out;
 if(value(during.allocated)<(4u<<20)+(haveHidden?(16u<<20):0))goto out;
 printf("global allocation delta=%lld (expected %u without concurrent clients)\n",(long long)(value(during.allocated)-value(before.allocated)),(4u<<20)+(haveHidden?(16u<<20):0));
 IOServiceClose(owner);owner=0;haveHidden=0;
 if(sample(observer,"owner closed",&after))goto out;
 failed=0;
out:
 if(owner)IOServiceClose(owner);
 if(observer)IOServiceClose(observer);
 IOObjectRelease(service);
 printf("%s global VRAM accounting\n",failed?"FAIL":"PASS");return failed;
}
