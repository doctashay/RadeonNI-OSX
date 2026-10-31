/* Request a conservative SMC policy or query live clocks. */
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
int main(int argc,char **argv)
{
 unsigned state=argc>1?strtoul(argv[1],0,0):0,what=argc>2?strtoul(argv[2],0,0):0,sclk=0,mclk=0,temp=0;io_connect_t conn;
 io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching(RDN_UC_SERVICE_CLASS));
 if(!service||IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE,&conn))return 1;
 int err=IOConnectMethodScalarIScalarO(conn,RDN_UC_POWER,2,3,state,what,&sclk,&mclk,&temp);
 printf("result=%08x core=%.2f memory=%.2f MHz temperature=%.1f C\n",err,sclk/100.0,mclk/100.0,((int)temp-RDN_UC_TEMPERATURE_BIAS)/1000.0);
 IOServiceClose(conn);IOObjectRelease(service);return err!=0;
}
