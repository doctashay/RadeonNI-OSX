/* Configure and observe the kernel's fence completion path. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
int main(int argc,char **argv)
{
 io_connect_t conn;unsigned flags=0,events=0,fallback=0,mode=argc>1?strtoul(argv[1],0,0):0;
 io_service_t service=rdn_probe_device(argc>2?argv[2]:NULL);
 if(!service||IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE_V2,&conn))return 1;
 int e=IOConnectMethodScalarIScalarO(conn,RDN_UC_FENCE_CONTROL,1,3,mode,&flags,&events,&fallback);
 printf("result=%08x irq=%u queued_surfaces=%u events=%u fallback=%u\n",e,flags&1,(flags>>1)&1,events,fallback);
 IOServiceClose(conn);IOObjectRelease(service);return e!=0;
}
