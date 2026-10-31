/* Verify framebuffer-index routing without submitting drawing commands. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOAccelSurfaceConnect.h>
#include "rdn_user.h"
int main(void)
{
 io_connect_t query=0,surface=0;
 io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching(RDN_UC_SERVICE_CLASS));
 if(!service || IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE_V2,&query) ||
    IOServiceOpen(service,mach_task_self(),kIOAccelSurfaceClientType,&surface)) return 1;
 unsigned wid=0x7fff2401,success=0,fail=0,offsets[6]={0};
 kern_return_t e=IOConnectMethodScalarIScalarO(surface,kIOAccelSurfaceSetIDMode,2,0,wid,0x24);
 if(e){printf("setID=%08x\n",e);return 1;}
 struct {IOAccelDeviceRegion region;IOAccelBounds extra;} shape;
 memset(&shape,0,sizeof(shape));shape.region.num_rects=1;
 shape.region.bounds.w=shape.region.bounds.h=1;
 shape.region.rect[0]=shape.region.bounds;
 for(unsigned i=0;i<7;++i){
  e=IOConnectMethodScalarIStructureI(surface,kIOAccelSurfaceSetShape,2,
    sizeof(shape),0xd,i,&shape);
  if(e){printf("head=%u shape=%08x\n",i,e);continue;}
  struct rdn_user_screen info;IOByteCount size=sizeof(info);
  e=IOConnectMethodScalarIStructureO(query,RDN_UC_SURFACE_SCREEN,1,&size,wid,&info);
  printf("head=%u query=%08x index=%u offset=%08x size=%ux%u pitch=%u scanout=%u\n",
    i,e,info.index,info.offset,info.width,info.height,info.pitch_pixels,info.scanout);
  if(e || i>=6 || info.index!=i || !info.width || !info.height || info.bits_per_pixel!=32){fail++;continue;}
  for(unsigned j=0;j<success;++j)if(offsets[j]==info.offset)fail++;
  offsets[success++]=info.offset;
 }
 IOServiceClose(surface);IOServiceClose(query);IOObjectRelease(service);
 printf("%s: %u framebuffer routes\n",fail||!success?"FAIL":"PASS",success);
 return fail||!success;
}
