/* A delayed producer must not quarantine the GPU during mirror refresh. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOAccelSurfaceConnect.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
#define CHECK(x) do { unsigned e=(x); if(e){printf("FAIL line %d: %08x\n",__LINE__,e);return 1;} } while(0)
static volatile uint32_t *signal_word;
static void *release_gpu(void *unused)
{
    usleep(900000);
    *signal_word=0xffffffffu;
    __asm__ volatile("sync":::"memory");
    return NULL;
}
static double now(void)
{
    struct timeval t;gettimeofday(&t,NULL);
    return t.tv_sec+t.tv_usec/1000000.0;
}
int main(int argc,char **argv)
{
    const unsigned id=0x7e002600,w=64,h=64,stride=256;
    io_service_t svc=rdn_probe_device(argc>1?argv[1]:"6704");
    io_connect_t device=0,surface=0;
    unsigned producer=0,mirror=0,command=0,fence=0,done=0;
    vm_address_t aperture=0;vm_size_t length=0;
    struct rdn_user_info info;IOByteCount size=sizeof(info);
    struct {IOAccelDeviceRegion region;IOAccelBounds rect;} shape;
    void *cpu=valloc(stride*h);
    setbuf(stdout,NULL);
    if(!svc||!cpu)return 2;
    memset(cpu,0,stride*h);
    CHECK(IOServiceOpen(svc,mach_task_self(),RDN_UC_TYPE_V2,&device));
    CHECK(IOConnectMethodScalarIStructureO(device,RDN_UC_GET_INFO,0,&size,&info));
    CHECK(IOConnectMapMemory(device,0,mach_task_self(),&aperture,&length,kIOMapAnywhere));
    CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_ALLOC,2,1,stride*h,4096,&producer));
    CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_ALLOC,2,1,stride*h,4096,&mirror));
    CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_ALLOC_COMMAND,2,1,4096,4096,&command));
    CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_SURFACE_BUFFER,5,0,id,0,0,0,0));
    CHECK(IOServiceOpen(svc,mach_task_self(),0,&surface));
    CHECK(IOConnectMethodScalarIScalarO(surface,kIOAccelSurfaceSetIDMode,2,0,id,0x24));
    memset(&shape,0,sizeof(shape));shape.region.num_rects=1;
    shape.region.bounds.w=w;shape.region.bounds.h=h;shape.rect=shape.region.bounds;
    CHECK(IOConnectMethodScalarIStructureI(surface,kIOAccelSurfaceSetShapeBackingAndLength,
          5,sizeof(shape),4,0,(unsigned)cpu,stride,stride*h,&shape));
    CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_SURFACE_GPU_COPY,5,0,id,producer,stride,w,h));
    CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_SURFACE_BUFFER,5,0,id,producer,stride,w,h));
    CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_SURFACE_GPU_COPY,5,0,id,mirror,stride,w,h));
    volatile uint32_t *ib=(void *)(aperture+command);
    volatile uint32_t *src=(void *)(aperture+producer),*dst=(void *)(aperture+mirror);
    signal_word=(void *)(aperture+command+256);
    uint64_t gpu=((uint64_t)info.vram_gpu_base_hi<<32)|info.vram_gpu_base_lo;
    gpu+=command+256;
    for(unsigned run=0;run<3;++run){
        pthread_t thread;*signal_word=0;src[0]=0x12345678u+run;
        ib[0]=0xc0053c00;ib[1]=0x13;ib[2]=(uint32_t)gpu;ib[3]=gpu>>32;
        ib[4]=ib[5]=0xffffffffu;ib[6]=10;
        for(unsigned i=7;i<16;++i)ib[i]=0x80000000;
        __asm__ volatile("sync":::"memory");
        CHECK(pthread_create(&thread,NULL,release_gpu,NULL));
        double start=now();
        unsigned submit=IOConnectMethodScalarIScalarO(device,RDN_UC_SUBMIT,2,1,command,16,&fence);
        double elapsed=now()-start;
        pthread_join(thread,NULL);CHECK(submit);
        CHECK(IOConnectMethodScalarIScalarO(device,RDN_UC_FENCE_WAIT,2,1,fence,3000,&done));
        printf("device=%04x run=%u submit_ms=%.3f fence=%u done=%u mirror=%08x\n",
               info.pci_device_id,run,elapsed*1000,fence,done,dst[0]);
        if(elapsed>0.5||!done||dst[0]!=src[0])return 3;
    }
    IOServiceClose(surface);IOServiceClose(device);IOObjectRelease(svc);free(cpu);
    puts("PASS delayed producer, self mirror, copied mirror and subsequent submissions");
    return 0;
}
