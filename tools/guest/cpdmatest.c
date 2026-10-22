/* Isolate CP DMA copies across GART page boundaries. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#define CHECK(x) do { unsigned e=(x); if(e){printf("line %u: 0x%x\n",__LINE__,e);return 1;} }while(0)
int main(int argc,char **argv)
{
    io_service_t service=IOServiceGetMatchingService(kIOMasterPortDefault,IOServiceMatching(RDN_UC_SERVICE_CLASS));
    io_connect_t conn;vm_address_t aperture=0,src=0,dst=0;vm_size_t aperture_size=0;
    unsigned sb=0,db=0,ib=0,on=0,start=0,pages=0,fence=0,done=0;
    unsigned iterations=argc>1?strtoul(argv[1],0,0):1;
    unsigned mode=argc>2?strtoul(argv[2],0,0):0;
    unsigned dma_flags=argc>3?strtoul(argv[3],0,0):0;
    unsigned first_size=argc>4?strtoul(argv[4],0,0):0x1a10;
    unsigned chunk=argc>5?strtoul(argv[5],0,0):0;
    const unsigned capacity=65536;unsigned char *source,*destination;
    struct rdn_user_info info;IOByteCount infosize=sizeof(info);
    setbuf(stdout,NULL);
    if(!service||!iterations||iterations>10000||mode>3||
       !first_size||first_size>capacity-8192||(dma_flags&~(1u<<21))||(chunk&&(chunk<16||chunk>4096||(chunk&(chunk-1)))))return 2;
    CHECK(IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE,&conn));
    CHECK(IOConnectMapMemory(conn,RDN_UC_MEMORY_APERTURE,mach_task_self(),&aperture,&aperture_size,kIOMapAnywhere));
    CHECK(IOConnectMethodScalarIStructureO(conn,RDN_UC_GET_INFO,0,&infosize,&info));
    CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_GART_INFO,0,3,&on,&start,&pages));
    if(!on)return 3;
    CHECK(vm_allocate(mach_task_self(),&src,capacity,VM_FLAGS_ANYWHERE));
    CHECK(vm_allocate(mach_task_self(),&dst,capacity,VM_FLAGS_ANYWHERE));
    CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_GART_BIND,2,1,(unsigned)src,capacity,&sb));
    CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_GART_BIND,2,1,(unsigned)dst,capacity,&db));
    CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_ALLOC,2,1,65536,4096,&ib));
    uint64_t gpu_source=(uint64_t)start+sb,gpu_destination=(uint64_t)start+db;
    source=(unsigned char *)src;destination=(unsigned char *)dst;
    if(mode&1){unsigned off;CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_ALLOC,2,1,capacity,4096,&off));gpu_source=((uint64_t)info.vram_gpu_base_hi<<32)+info.vram_gpu_base_lo+off;source=(unsigned char *)(aperture+off);}
    if(mode&2){unsigned off;CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_ALLOC,2,1,capacity,4096,&off));gpu_destination=((uint64_t)info.vram_gpu_base_hi<<32)+info.vram_gpu_base_lo+off;destination=(unsigned char *)(aperture+off);}
    printf("copy mode %u source 0x%llx destination 0x%llx\n",mode,(unsigned long long)gpu_source,(unsigned long long)gpu_destination);
    volatile unsigned *words=(volatile unsigned *)(aperture+ib);
    unsigned frame,i;
    for(frame=0;frame<iterations;++frame){
        unsigned so=frame?((frame*37)&8191):0xff0;
        unsigned to=frame?((frame*13)&8191):0x430;
        unsigned bytes=frame?1+((frame*277)%32768):first_size;
        uint64_t sa=gpu_source+so,da=gpu_destination+to;
        for(i=0;i<capacity;++i){source[i]=(i*17+frame*31)&255;destination[i]=0xa5;}
        __asm__ volatile("sync":::"memory");
        unsigned data[16384]={0xc0034300,0x1980003c,0xffffffff,0,10};
        unsigned count=5,left=bytes;
        while(left){
            unsigned part=left;
            if(chunk){
                unsigned sr=chunk-((unsigned)sa&(chunk-1)),dr=chunk-((unsigned)da&(chunk-1));
                if(part>sr)part=sr;
                if(part>dr)part=dr;
            }
            if(count+8>16384)return 6;
            data[count++]=0xc0044100;data[count++]=(unsigned)sa;
            data[count++]=0x80000000|((unsigned)(sa>>32)&255);
            data[count++]=(unsigned)da;data[count++]=(unsigned)(da>>32)&255;
            data[count++]=part|dma_flags;
            sa+=part;da+=part;left-=part;
        }
        data[count++]=0xc0004200;data[count++]=0;
        for(i=0;i<count;++i)words[i]=data[i];
        __asm__ volatile("sync":::"memory");
        CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_SUBMIT,2,1,ib,count,&fence));
        CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_FENCE_WAIT,2,1,fence,3000,&done));
        if(!done){printf("STALL frame %u fence %u source +0x%x destination +0x%x bytes 0x%x\n",frame,fence,so,to,bytes);return 4;}
        CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_SYNC_FOR_CPU,0,0));
        for(i=0;i<capacity;++i){unsigned expected=i>=to&&i<to+bytes?source[so+i-to]:0xa5;if(destination[i]!=expected){printf("MISMATCH frame %u byte %u got %u expected %u\n",frame,i,destination[i],expected);return 5;}}
        if(frame%100==0)printf("completed %u fence %u\n",frame+1,fence);
    }
    CHECK(IOServiceClose(conn));IOObjectRelease(service);
    vm_deallocate(mach_task_self(),src,capacity);vm_deallocate(mach_task_self(),dst,capacity);
    printf("PASS %u copies with byte checks\n",iterations);return 0;
}
