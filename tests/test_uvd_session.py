"""Run the production video session and mapped-memory lifetime code with fake hardware."""
import os,pathlib,shlex,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
PREFIX=r"""
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <strings.h>
extern "C" {
#include "rdn_video.h"
#include "rdn_user.h"
#include "rdn_uvd.h"
#include "rdn_mem.h"
#include "rdn_gart.h"
#include "rdn_cp_dma_readback.h"
}
using UInt8=uint8_t; using UInt32=uint32_t; using UInt64=uint64_t; using SInt32=int32_t;
using IOReturn=int; using IOOptionBits=uint32_t; using IOByteCount=size_t;
using IOPhysicalAddress=uint32_t; using addr64_t=uint64_t;
using IOVirtualAddress=uintptr_t; using vm_map_t=void*;
enum { kIOReturnSuccess,kIOReturnBadArgument,kIOReturnNotReady,kIOReturnNoMemory,
kIOReturnUnsupported,kIOReturnVMError,kIOReturnBusy,kIOReturnNoResources,kIOReturnIOError,kIOReturnNotPermitted };
enum { kIODirectionInOut=3,kIOMapDefaultCache=0,kIOMapCacheMask=0xf00,kIOMapReadOnly=0x1000 };
struct Calls {
 std::vector<std::string> feedbackLogs,timingLogs,failureLogs,memoryLogs; UInt64 clockReads=0;
 UInt32 feedbackStatus=0;bool staleFeedback=false;
 int alloc=0,freed=0,submits=0,polls=0,sleeps=0,wake=0,idle=0;
 int gartAlloc=0,gartFree=0,gartFlush=0,cpSubmit=0,cpPoll=0; uint64_t readBytes=0;
 int initSubmit=0,initPoll=0,failInitSubmit=0,failInitWait=0,busyInit=0;
 bool initDone=false,retireInit=false;
 int failGartFlush=0,failCPSubmit=0,failCPWait=0,busyCP=0;
 bool retireBeforeCP=false; int failBusPage=-1;
 bool cpInflight=false;
 int dmaNew=0,dmaSet=0,dmaClear=0,dmaRelease=0,failDMA=0,badSegment=0;
 int failSubmit=0,failWait=0,busySubmit=0,wakeError=0,idleError=0;
 bool inflight=false,corrupt=false,retireBeforeSubmit=false,allocFail=false,metadataFail=false;
 uint64_t allocated=0x400000,busBase=0x10000000,physicalBase=0x212340000ull,lastPTE=0;
} calls;
thread_local int gfxDepth=0,uvdDepth=0;
static std::mutex waitMutex;static std::condition_variable waitChanged;static bool holdWait=false,waitEntered=false,releaseWait=false;
struct IOLock { std::mutex mutex; bool gfx; IOLock(bool g):gfx(g){} };
void IOLockLock(IOLock *p) { if(!p->gfx) assert(!gfxDepth); p->mutex.lock(); if(p->gfx) ++gfxDepth; else ++uvdDepth; }
bool IOLockTryLock(IOLock *p) { assert(!gfxDepth&&!uvdDepth);if(!p->mutex.try_lock())return false;if(p->gfx)++gfxDepth;else ++uvdDepth;return true; }
void IOLockUnlock(IOLock *p) { if(p->gfx) --gfxDepth; else --uvdDepth; p->mutex.unlock(); }
void *IOMalloc(size_t n) { return calls.metadataFail?nullptr:malloc(n); }
void IOFree(void *p,size_t) { free(p); }
void *IOMallocAligned(size_t n,size_t align) { void *p=0; return posix_memalign(&p,align,n)?nullptr:p; }
void IOFreeAligned(void *p,size_t) { free(p); }
bool OSCompareAndSwap(UInt32 a,UInt32 b,volatile UInt32 *p) { return __sync_bool_compare_and_swap(p,a,b); }
void OSAddAtomic(SInt32 n,volatile SInt32 *p) { __sync_fetch_and_add(p,n); }
void OSSynchronizeIO() {}
void rdnStoreBarrier() {}
void IOLog(const char *format,...){
 assert(!gfxDepth&&uvdDepth==1);
 char buffer[512];va_list args;va_start(args,format);
 int n=vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);
 assert(n>0&&unsigned(n)<sizeof(buffer));
 if(strstr(format,"UVD memory"))calls.memoryLogs.push_back(buffer);else if(strstr(format,"create failed"))calls.failureLogs.push_back(buffer);else if(strstr(format,"timing"))calls.timingLogs.push_back(buffer);else calls.feedbackLogs.push_back(buffer);
}
void clock_get_uptime(UInt64 *value){*value=++calls.clockReads*1000;}
void absolutetime_to_nanoseconds(UInt64 value,UInt64 *nanos){*nanos=value;}

void IOSleep(unsigned n) { assert(!gfxDepth && uvdDepth==1 && n==1); ++calls.sleeps; std::unique_lock<std::mutex> lock(waitMutex); if(holdWait){waitEntered=true;waitChanged.notify_all();waitChanged.wait(lock,[]{return releaseWait;});} }
struct OSObject { virtual ~OSObject(){} } truth;
OSObject *kOSBooleanTrue=&truth;
struct OSNumber:OSObject { UInt32 value=1; UInt32 unsigned32BitValue() { return value; } } version;
#define OSDynamicCast(t,p) dynamic_cast<t*>(p)
struct Service {
 bool production=false,validation=true,marker=true,gpu=false;
 OSObject *getProperty(const char *name) {
  std::string n(name);
  if(n=="EnableUVDGPUOutput") return gpu?kOSBooleanTrue:nullptr;
  if(n=="EnableUVDSessions") return production?kOSBooleanTrue:nullptr;
  if(n=="EnableUVDValidation") return validation?kOSBooleanTrue:nullptr;
  if(n=="UVDValidatedAPI") return &version;
  return marker?kOSBooleanTrue:nullptr;
 }
};
#define RDN_UVD_REBOOT_PROPERTY "RadeonNIUVDRebootRequired"
class IOGeneralMemoryDescriptor {
 unsigned refs=1; void *bytes=nullptr; size_t length=0;
public:
 static void *operator new(size_t n) { return calloc(1,n); }
 static void operator delete(void *p) { ::free(p); }
 bool prepared=false;
 virtual ~IOGeneralMemoryDescriptor(){}
 void retain(){++refs;} void release(){assert(refs);if(!--refs)free();}
 virtual void free(){assert(!prepared);delete this;}
 IOReturn prepare(){prepared=true;return 0;}
 IOReturn complete(){prepared=false;return 0;}
 addr64_t getPhysicalSegment64(IOByteCount off,IOByteCount *n){*n=4096;return calls.physicalBase+off;}
 bool initWithAddress(void *b,size_t n,int){bytes=b;length=n;return b&&n;}
 size_t getLength(){return length;}
 virtual IOReturn doMap(vm_map_t,IOVirtualAddress*,IOOptionBits,IOByteCount=0,IOByteCount=0){return kIOReturnSuccess;}
};
using IOMemoryDescriptor=IOGeneralMemoryDescriptor;
class IODMACommand {
 IOMemoryDescriptor *memory=nullptr;
public:
 enum {OutputHost64=1,kMapped=2,kIterateOnly=4};using MappingOptions=unsigned;
 struct Segment64 {UInt64 fIOVMAddr,fLength;};
 static IODMACommand *withSpecification(int format,unsigned bits,UInt64 segment,MappingOptions flags,UInt64 transfer,UInt64 align){
  assert(!gfxDepth&&format==OutputHost64&&bits==40&&segment==4096&&flags==(kMapped|kIterateOnly)&&transfer&&transfer<4*1024*1024&&align==4096);
  ++calls.dmaNew;return calls.failDMA==1?nullptr:new IODMACommand;
 }
 IOReturn setMemoryDescriptor(IOMemoryDescriptor *m){assert(!gfxDepth&&m&&m->prepared&&!memory);++calls.dmaSet;memory=m;memory->retain();return calls.failDMA==2?kIOReturnVMError:0;}
 IOReturn gen64IOVMSegments(UInt64 *off,Segment64 *s,UInt32 *count){
  assert(gfxDepth==1&&memory&&memory->prepared&&*count==1&&*off%4096==0);
  if(int(*off/4096)==calls.failBusPage)return kIOReturnVMError;
  s->fIOVMAddr=calls.busBase+*off;s->fLength=4096;*off+=4096;
  if(calls.badSegment==1)*count=0;if(calls.badSegment==2)--*off;
  if(calls.badSegment==3)--s->fLength;if(calls.badSegment==4)++s->fIOVMAddr;
  return 0;
 }
 IOReturn clearMemoryDescriptor(){assert(!gfxDepth&&!calls.cpInflight&&calls.gartAlloc==calls.gartFree);++calls.dmaClear;if(calls.failDMA==3)return kIOReturnVMError;if(memory){memory->release();memory=nullptr;}return 0;}
 void release(){assert(!memory);++calls.dmaRelease;delete this;}
};
#define OSDeclareDefaultStructors(x)
class RadeonNIVideoUserClient {};
struct RadeonNIVideoSession;
class RadeonNIAccel {
public:
 Service fb,provider; Service *fFramebuffer=&fb,*fUvdProvider=&provider;
 IOLock gfx{true},uvd{false}; IOLock *fLock=&gfx,*fUvdLock=&uvd;
 bool fRetiring=false,fEngineUp=true,fVideoBusy=false;
 rdn_accel fAccel{}; rdn_mem fMem{},fHidden{},fGartSpace{}; rdn_gart fGart{}; rdn_uvd fUvd{};
 UInt32 fUvdState=1,fUvdUsers=0,fUvdLive=0,fUvdScratch=0; SInt32 fUvdError=0;
 RadeonNIVideoSession *fVideoSession=nullptr; UInt64 fVideoEpoch=0;
 volatile UInt32 fVideoCpuBytes=0; unsigned refs=1;
 UInt32 fHiddenOffset=0x10000000,fHiddenSize=128*1024*1024;
 std::vector<UInt32> aperture,hidden;
 RadeonNIAccel(); ~RadeonNIAccel();
 void retain(){++refs;} void release(){assert(refs>1);--refs;}
 IOReturn uvdAcquireRaw(bool*);
 bool videoCharge(UInt32); void videoUncharge(UInt32);
 bool videoReady(bool,bool); void videoQuery(bool,rdn_video_query*);
 void videoPoison(int); int videoCommand(UInt32,const rdn_uvd_h264_picture*,UInt32);
 void videoCleanup(bool);
 int videoBindReadback();int videoUnbindReadback();int videoReadback(bool=false);
 int videoSubmitGfx(UInt32,bool);int videoInitializeVram();
 IOReturn videoCreate(RadeonNIVideoUserClient*,bool,const rdn_video_create*,rdn_video_session*);
 IOReturn videoDecode(RadeonNIVideoUserClient*,const rdn_video_decode*,rdn_video_output*);
 IOReturn videoDecodeGpu(RadeonNIVideoUserClient*,const rdn_video_decode*,rdn_video_gpu_output*);
 IOReturn videoDecodeImpl(RadeonNIVideoUserClient*,const rdn_video_decode*,rdn_video_output*,rdn_video_gpu_output*);
 IOReturn videoAcquireGpu(RadeonNIVideoUserClient*,const rdn_video_gpu_acquire*,rdn_video_gpu_source*);
 IOReturn videoRetireGpu(RadeonNIVideoUserClient*,const rdn_video_gpu_retire*);
 IOReturn videoRelease(RadeonNIVideoUserClient*,const rdn_video_release*);
 IOReturn videoDestroy(RadeonNIVideoUserClient*,const rdn_video_destroy*);
 void videoClose(RadeonNIVideoUserClient*);
 IOReturn videoMemory(RadeonNIVideoUserClient*,UInt32,IOOptionBits*,IOMemoryDescriptor**);
};
static RadeonNIAccel *active;
extern "C" {
 int actual_mem_alloc(rdn_mem*,uint64_t,uint64_t,uint64_t*);
 int actual_mem_alloc_in_range(rdn_mem*,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t*);
 void actual_mem_free(rdn_mem*,uint64_t);
}
static void *heapAlloc(void*,size_t n){return calloc(1,n);}
static void heapFree(void*,void*p){free(p);}
static rdn_os heapOS;

"""
SUFFIX=r"""
RadeonNIAccel::RadeonNIAccel():aperture(64*1024*1024/4),hidden(fHiddenSize/4) {
 calls=Calls(); active=this; heapOS={};heapOS.alloc=heapAlloc;heapOS.free=heapFree;
 assert(!rdn_mem_init(&fMem,&heapOS,0x400000,60*1024*1024));
 assert(!rdn_mem_init(&fHidden,&heapOS,fHiddenOffset,fHiddenSize));fGart.ready=true; fAccel.aperture=aperture.data();fAccel.aperture_size=aperture.size()*4;
 fUvd.guarded=true;fUvd.ready=true;fUvd.vcpu_gpu=0x100000;fUvd.vcpu_bytes=0x80000;fUvd.ring_gpu=0x200000;fUvd.ring_words=1024;
}
RadeonNIAccel::~RadeonNIAccel(){videoClose(nullptr);if(fVideoSession){calls.inflight=calls.cpInflight=false;if(fVideoSession->readbackDMA){calls.gartFree=calls.gartAlloc;calls.failDMA=0;fVideoSession->readbackDMA->clearMemoryDescriptor();fVideoSession->readbackDMA->release();}if(fVideoSession->readback){fVideoSession->readback->complete();fVideoSession->readback->release();}IOFree(fVideoSession,sizeof(*fVideoSession));fVideoSession=nullptr;}assert(!fVideoCpuBytes&&refs==1);rdn_mem_fini(&fMem);rdn_mem_fini(&fHidden);}
extern "C" int rdn_mem_alloc(rdn_mem *mem,uint64_t bytes,uint64_t align,uint64_t *out){assert(gfxDepth==1&&align==4096);if(mem==&active->fGartSpace){++calls.gartAlloc;*out=4096;return 0;}++calls.alloc;if(calls.allocFail)return -ENOMEM;return actual_mem_alloc(mem,bytes,align,out);}
extern "C" int rdn_mem_alloc_in_range(rdn_mem *mem,uint64_t bytes,uint64_t align,uint64_t lo,uint64_t hi,uint64_t *out){assert(gfxDepth==1&&align==4096);++calls.alloc;if(calls.allocFail)return -ENOMEM;return actual_mem_alloc_in_range(mem,bytes,align,lo,hi,out);}
extern "C" void rdn_mem_free(rdn_mem *mem,uint64_t at){assert(gfxDepth==1&&!calls.inflight&&!calls.cpInflight);if(mem==&active->fGartSpace){assert(at==4096);++calls.gartFree;return;}++calls.freed;actual_mem_free(mem,at);}
static UInt8 *gpuBytes(UInt64 address,UInt32 bytes){
 assert(address>=active->fAccel.vram_base);UInt64 offset=address-active->fAccel.vram_base;
 if(offset<active->aperture.size()*4){assert(bytes<=active->aperture.size()*4-offset);return (UInt8*)active->aperture.data()+offset;}
 assert(offset>=active->fHiddenOffset&&offset-active->fHiddenOffset<=active->hidden.size()*4&&bytes<=active->hidden.size()*4-(offset-active->fHiddenOffset));
 return (UInt8*)active->hidden.data()+offset-active->fHiddenOffset;
}
extern "C" void rdn_hdp_flush(rdn_accel*){if(calls.retireBeforeSubmit){active->fRetiring=true;calls.retireBeforeSubmit=false;}}
extern "C" int rdn_uvd_wake(rdn_uvd*){assert(!gfxDepth&&uvdDepth==1);++calls.wake;return calls.wakeError;}
extern "C" int rdn_uvd_idle(rdn_uvd*){assert(!gfxDepth&&uvdDepth==1&&!calls.inflight);++calls.idle;return calls.idleError;}
extern "C" int rdn_uvd_ib_try_submit(rdn_uvd *u,uint64_t, uint32_t, uint32_t *seq){
 assert(gfxDepth==1&&uvdDepth==1&&!active->fRetiring&&!calls.inflight&&calls.initDone);++calls.submits;
 if(calls.busySubmit==calls.submits)return -EBUSY;
 calls.inflight=true;*seq=++u->fence_emitted;
 return calls.failSubmit==calls.submits?-EIO:0;
}
extern "C" bool rdn_uvd_fence_done(rdn_uvd*,uint32_t){
 assert(gfxDepth==1&&uvdDepth==1);++calls.polls;
 if(calls.failWait==calls.submits)return false;
 calls.inflight=false;
 RadeonNIVideoSession *s=active->fVideoSession;
 if(s && rdn_get_le32(s->message,4)==1){
  UInt8 *base=(UInt8*)active->fAccel.aperture+s->allocation;
  for(UInt32 row=0;row<s->config.coded_height;++row){
   UInt32 off=(row&1)?s->layout.luma_bottom_offset:s->layout.luma_top_offset;
   off+=(row/2)*s->layout.target_pitch;
   memset(gpuBytes(s->addresses[kVideoTarget]+off,s->config.coded_width),(row+calls.submits)&255,s->config.coded_width);
  }
  for(UInt32 row=0;row<s->config.coded_height/2;++row){
   UInt32 off=(row&1)?s->layout.chroma_bottom_offset:s->layout.chroma_top_offset;
   off+=(row/2)*s->layout.target_pitch;
   memset(gpuBytes(s->addresses[kVideoTarget]+off,s->config.coded_width),(row+97+calls.submits)&255,s->config.coded_width);
  }

  for(unsigned i=0;i<8;++i){
   UInt32 values[8]={2048,s->nextRequest,calls.feedbackStatus,1,0x01020308,0x01020309,0x0102030a,0x0102030b};
   if(calls.staleFeedback)values[1]=s->nextRequest-1;
   UInt32 value=values[i];
   for(unsigned j=0;j<4;++j)base[s->offsets[kVideoFeedback]+4*i+j]=UInt8(value>>(8*j));
  }
  if(calls.corrupt)*gpuBytes(s->addresses[kVideoTarget]-1,1)=0;
  if(calls.retireBeforeCP)active->fRetiring=true;
 }
 return true;
}

extern "C" void rdn_gart_set_page(rdn_gart*,uint32_t page,uint64_t bus){assert(gfxDepth==1&&page&&bus==calls.busBase+(page-1)*4096ull&&!(bus&4095));calls.lastPTE=bus;}
extern "C" void rdn_gart_clear_page(rdn_gart*,uint32_t page){assert(gfxDepth==1&&page);}
extern "C" int rdn_gart_flush_checked(rdn_gart*){assert(gfxDepth==1);++calls.gartFlush;return calls.failGartFlush==calls.gartFlush?-ETIMEDOUT:0;}
extern "C" int rdn_ib_try_submit(rdn_accel*,uint64_t address,uint32_t count,uint32_t *fence){
 assert(gfxDepth==1&&uvdDepth==1&&!calls.inflight&&!calls.cpInflight&&!active->fRetiring);
 assert(!active->fVideoSession->gfxPending);
 if(!active->fVideoSession->created){
  ++calls.initSubmit;
  if(calls.retireInit)active->fRetiring=true;
  if(calls.initSubmit<=calls.busyInit)return -EBUSY;
  assert(count<=1024);calls.cpInflight=true;*fence=42;
  return calls.failInitSubmit?-EIO:0;
 }
 ++calls.cpSubmit;if(calls.cpSubmit<=calls.busyCP)return -EBUSY;
 calls.cpInflight=true;*fence=42;
 auto*s=active->fVideoSession;assert(address==s->buffers.ib.address&&count<=128);
 return calls.failCPSubmit==calls.cpSubmit?-EIO:0;
}
extern "C" bool rdn_fence_done(rdn_accel*,uint32_t fence){
 assert(gfxDepth==1&&uvdDepth==1&&fence==42);++calls.cpPoll;
 if(!active->fVideoSession->created){
  ++calls.initPoll;if(calls.failInitWait)return false;
  auto*s=active->fVideoSession;const UInt32 *words=(const UInt32*)((const UInt8*)active->fAccel.aperture+s->allocation+s->offsets[kVideoIB]);
  assert(words[0]==0xc0034300u);unsigned index=5,filled=0;
  while(words[index]==0xc0044100u){
   UInt32 count=words[index+5];uint64_t dst=words[index+3]|((uint64_t)words[index+4]<<32);
   assert((words[index+2]&0x7fffffff)==0x40000000u&&count&&count<=1048576);
   assert((dst>=s->allocatedAddress&&dst-s->allocatedAddress+count<=s->allocationBytes)||(dst>=s->gpuAllocation[0]&&dst-s->gpuAllocation[0]+count<=s->gpuAllocationBytes[0])||(dst>=s->gpuAllocation[1]&&dst-s->gpuAllocation[1]+count<=s->gpuAllocationBytes[1]));
   assert(dst+count<=s->buffers.ib.address||dst>=s->buffers.ib.address+s->buffers.ib.bytes);
   UInt8 *p=gpuBytes(dst,count);
   for(unsigned i=0;i<count;++i)p[i]=UInt8(words[index+1]>>(8*(i&3)));
   filled+=count;index+=6;
  }
  assert(filled==s->allocationBytes+s->gpuAllocationBytes[0]+s->gpuAllocationBytes[1]-4096&&words[index]==0xc0004200u);
  for(unsigned span=0;span<kVideoSpans;++span){
   UInt8 *p=gpuBytes(s->addresses[span]-4096,s->sizes[span]+8192)+4096;
   for(unsigned i=0;i<4096;++i)assert(p[int(i)-4096]==0xa5&&p[s->sizes[span]+i]==0xa5);
   if(span!=kVideoIB)for(unsigned i=0;i<s->sizes[span];++i)assert(p[i]==0);
  }
  calls.initDone=true;calls.cpInflight=false;return true;
 }
 if(calls.failCPWait==calls.cpSubmit)return false;
 auto*s=active->fVideoSession;const UInt32 *words=(const UInt32*)((const UInt8*)active->fAccel.aperture+s->allocation+s->offsets[kVideoIB]);
 assert(words[0]==0xc0034300u);unsigned index=5,copied=0;
 while(words[index]==0xc0044100u){
  uint64_t src=words[index+1]|((uint64_t)(words[index+2]&255)<<32);
  uint64_t dst=words[index+3]|((uint64_t)words[index+4]<<32);UInt32 count=words[index+5];
  assert((src>=s->allocatedAddress&&src-s->allocatedAddress+count<=s->allocationBytes)||(src>=s->gpuAllocation[0]&&src-s->gpuAllocation[0]+count<=s->gpuAllocationBytes[0])||(src>=s->gpuAllocation[1]&&src-s->gpuAllocation[1]+count<=s->gpuAllocationBytes[1]));
  assert(dst>=RDN_GART_GPU_START+s->gartOffset&&dst-RDN_GART_GPU_START-s->gartOffset+count<=s->readbackBytes);
  memcpy((UInt8*)s->readback->getBytesNoCopy()+dst-RDN_GART_GPU_START-s->gartOffset,gpuBytes(src,count),count);
  calls.readBytes+=count;++copied;index+=6;
 }
 assert(copied>=(s->gpuOutput?12:13)&&words[index]==0xc0004200u);calls.cpInflight=false;return true;
}
static rdn_video_create config(){
 rdn_video_create c{};c.version=1;c.bytes=sizeof(c);c.flags=RDN_VIDEO_CREATE_VALIDATION;
 c.config.version=1;c.config.profile_idc=100;c.config.level_idc=30;
 c.config.coded_width=864;c.config.coded_height=480;c.config.max_num_ref_frames=2;c.config.chroma_format_idc=1;c.config.frame_mbs_only_flag=1;return c;
}
static rdn_video_decode picture(const rdn_video_session &s,UInt32 request=1){
 rdn_video_decode d{};d.version=1;d.bytes=sizeof(d);d.session=s.session;d.request_sequence=request;d.bitstream_bytes=1408;
 d.picture.version=1;d.picture.sps_info_flags=5;d.picture.pps_info_flags=0x88;d.picture.log2_max_frame_num_minus4=1;d.picture.log2_max_pic_order_cnt_lsb_minus4=3;d.picture.pic_init_qp_minus26=2;
 memset(d.picture.scaling_list_4x4,16,sizeof(d.picture.scaling_list_4x4));memset(d.picture.scaling_list_8x8,16,sizeof(d.picture.scaling_list_8x8));return d;
}
static rdn_video_release release(const rdn_video_output &o){rdn_video_release r{};r.version=1;r.bytes=sizeof(r);r.session=o.session;r.output_sequence=o.output_sequence;r.slot=o.slot;return r;}
static rdn_video_destroy destroy(const rdn_video_session &s){rdn_video_destroy d{};d.version=1;d.bytes=sizeof(d);d.session=s.session;return d;}
static rdn_video_session create(RadeonNIAccel&a,RadeonNIVideoUserClient&owner){auto c=config();rdn_video_session s{};assert(a.videoCreate(&owner,true,&c,&s)==kIOReturnSuccess);assert(s.output_bytes==622080&&s.upload_bytes==4194304&&s.output_slots==2);memset(a.fVideoSession->upload->getBytesNoCopy(),0x65,1408);return s;}
static void query(){RadeonNIAccel a;rdn_video_query q{};a.videoQuery(false,&q);assert(!q.flags&&!q.profile_mask&&!calls.alloc);a.videoQuery(true,&q);assert(q.flags==(RDN_VIDEO_CAP_READY|RDN_VIDEO_CAP_VALIDATION));assert(!calls.alloc&&!calls.submits);a.fb.production=true;a.videoQuery(false,&q);assert(q.flags==(RDN_VIDEO_CAP_READY|RDN_VIDEO_CAP_PRODUCTION));}
static void slots(){RadeonNIAccel a;RadeonNIVideoUserClient owner,other;auto s=create(a,owner);auto d=picture(s);rdn_video_output one{},two{},three{};assert(a.videoDecode(&other,&d,&one)==kIOReturnNotReady);assert(a.videoDecode(&owner,&d,&one)==0);assert(one.output_sequence==1&&one.slot==0);auto old=std::vector<UInt8>((UInt8*)a.fVideoSession->output[0]->getBytesNoCopy(),(UInt8*)a.fVideoSession->output[0]->getBytesNoCopy()+one.output_bytes);for(UInt32 row=0;row<480;++row)for(UInt32 x=0;x<864;++x)assert(old[row*864+x]==((row+2)&255));for(UInt32 row=0;row<240;++row)for(UInt32 x=0;x<864;++x)assert(old[864*480+row*864+x]==((row+99)&255));d.request_sequence=2;assert(a.videoDecode(&owner,&d,&two)==0&&two.slot==1&&two.output_sequence==2);assert(!memcmp(old.data(),a.fVideoSession->output[0]->getBytesNoCopy(),old.size()));d.request_sequence=3;int submitted=calls.submits;assert(a.videoDecode(&owner,&d,&three)==kIOReturnBusy&&calls.submits==submitted);auto r=release(one);assert(a.videoRelease(&owner,&r)==0);assert(a.videoRelease(&owner,&r)==kIOReturnBadArgument);assert(a.videoDecode(&owner,&d,&three)==0&&three.slot==0&&three.output_sequence==3);auto x=destroy(s);assert(a.videoDestroy(&owner,&x)==0&&calls.freed==3&&!a.fVideoSession);}
static void lifetime(){RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);IOOptionBits options=0;IOMemoryDescriptor *memory=nullptr;assert(a.videoMemory(&owner,RDN_VIDEO_MEMORY_OUTPUT0,&options,&memory)==0&&(options&kIOMapReadOnly));auto *mapped=static_cast<RadeonNIVideoMemory*>(memory);IOVirtualAddress address=0;assert(mapped->doMap(nullptr,&address,options)==0);assert(mapped->doMap(nullptr,&address,options|0x100)==kIOReturnNotPermitted);assert(mapped->doMap(nullptr,&address,0)==kIOReturnNotPermitted);auto charged=memory->getLength();a.videoClose(&owner);assert(a.fVideoCpuBytes==charged&&a.refs==2&&!a.fVideoSession);memory->release();assert(a.fVideoCpuBytes==0&&a.refs==1);}
static void retired(){RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);auto d=picture(s);rdn_video_output out{};calls.retireBeforeSubmit=true;assert(a.videoDecode(&owner,&d,&out)==kIOReturnNotReady);assert(calls.submits==1&&!out.output_sequence);a.videoClose(&owner);assert(a.fVideoSession&&a.fVideoSession->quarantined&&!calls.freed);}
static void errors(int which){RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);auto d=picture(s);rdn_video_output out{};if(which==0)calls.failSubmit=2;if(which==1)calls.failWait=2;if(which==2)calls.corrupt=true;assert(a.videoDecode(&owner,&d,&out)==kIOReturnIOError&&!out.output_sequence);a.videoClose(&owner);assert(a.fVideoSession&&a.fVideoSession->quarantined&&!calls.freed&&a.fVideoCpuBytes==a.fVideoSession->readbackBytes);calls.inflight=false;}
static void busy(){RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);auto d=picture(s);rdn_video_output out{};calls.busySubmit=2;assert(a.videoDecode(&owner,&d,&out)==kIOReturnBusy&&!a.fUvd.failed&&!a.fVideoSession->quarantined&&a.fVideoSession->nextRequest==1);assert(a.videoDecode(&owner,&d,&out)==0&&out.request_sequence==1);}
static void quota(){RadeonNIAccel a;std::vector<RadeonNIVideoMemory*> held;for(int i=0;i<5;++i){auto*p=RadeonNIVideoMemory::withBytes(&a,4*1024*1024,false);assert(p);held.push_back(p);}assert(!RadeonNIVideoMemory::withBytes(&a,1,false));RadeonNIVideoUserClient owner;auto c=config();rdn_video_session s{};assert(a.videoCreate(&owner,true,&c,&s)==kIOReturnNoMemory&&!calls.submits&&calls.freed==3);for(auto*p:held)p->release();assert(!a.fVideoCpuBytes);}
static void invalid(int which){RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session s{};if(which==0)c.flags=2;if(which==1)c.reserved[3]=1;if(which==2)c.config.frame_mbs_only_flag=0;if(which==3)c.config.coded_width=0;if(which==4)a.fb.validation=false;assert(a.videoCreate(&owner,true,&c,&s)!=0&&!calls.alloc&&!calls.submits);}

static void creationFailure(int mode){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session out{};
 if(mode==0)calls.failGartFlush=1;if(mode==1)calls.failBusPage=2;if(mode==2)calls.wakeError=-EIO;if(mode==3)calls.failSubmit=1;if(mode==4)calls.busySubmit=1;
 assert(a.videoCreate(&owner,true,&c,&out)!=0&&!out.session.low);
 if(mode==0||mode==3){assert(a.fVideoSession&&a.fVideoSession->quarantined&&!calls.freed&&a.fVideoCpuBytes==a.fVideoSession->readbackBytes);}
 else {assert(!a.fVideoSession&&calls.freed==3&&calls.gartFree==1&&!a.fVideoCpuBytes);}
}
static void readbackFailure(int mode){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);auto d=picture(s);rdn_video_output out{};
 if(mode==0)calls.failCPSubmit=1;if(mode==1)calls.failCPWait=1;if(mode==2)calls.retireBeforeCP=true;
 if(mode==3)calls.busyCP=2000;if(mode==4){calls.busyCP=3;calls.failCPWait=4;}
 assert(a.videoDecode(&owner,&d,&out)!=0&&!out.output_sequence);
 if(mode>=3)assert(calls.sleeps==1000&&calls.submits==2);
 if(mode==3)assert(!a.fVideoSession->gfxPending&&!calls.cpInflight&&calls.cpSubmit==1001);
 a.videoClose(&owner);assert(a.fVideoSession&&a.fVideoSession->quarantined&&!calls.freed&&!calls.gartFree);
 assert(a.fVideoSession->readbackPrepared&&a.fVideoCpuBytes==a.fVideoSession->readbackBytes);
 assert(a.fVideoSession->readbackDMA&&calls.dmaClear==0&&calls.dmaRelease==0);
}
static void readbackBusy(){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);auto d=picture(s);rdn_video_output out{};
 calls.busyCP=3;assert(a.videoDecode(&owner,&d,&out)==0&&out.request_sequence==1);
 assert(calls.cpSubmit==4&&calls.sleeps==3&&calls.submits==2&&!a.fUvd.failed&&!a.fVideoSession->gfxPending);
}
static void physical64(){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);
 assert(calls.lastPTE==calls.busBase+a.fVideoSession->readbackBytes-4096&&calls.lastPTE<UINT32_MAX&&calls.physicalBase>UINT32_MAX);
 assert(calls.lastPTE!=calls.physicalBase+a.fVideoSession->readbackBytes-4096);
 a.videoClose(&owner);assert(!a.fVideoSession&&!a.fVideoCpuBytes);
 assert(calls.dmaClear==1&&calls.dmaRelease==1);
 calls.busBase=0x312340000ull;s=create(a,owner);
 assert(calls.lastPTE==calls.busBase+a.fVideoSession->readbackBytes-4096&&calls.lastPTE>UINT32_MAX);
 a.videoClose(&owner);
 calls.busBase=1ull<<40;auto c=config();rdn_video_session out{};int submitted=calls.submits;
 assert(a.videoCreate(&owner,true,&c,&out)==kIOReturnVMError&&!a.fVideoSession&&calls.submits==submitted);
}
static void dmaFailure(int mode){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session out{};
 if(mode<2)calls.failDMA=mode+1;else calls.badSegment=mode-1;
 assert(a.videoCreate(&owner,true,&c,&out)==kIOReturnVMError&&!a.fVideoSession&&!calls.submits&&!a.fVideoCpuBytes);
 assert(calls.freed==3&&calls.gartAlloc==calls.gartFree);
}
static void queryBusy(){
 RadeonNIAccel a;rdn_video_query q{};IOLockLock(a.fUvdLock);
 std::thread query([&]{a.videoQuery(true,&q);});query.join();
 assert(q.version==1&&q.bytes==sizeof(q)&&q.flags==RDN_VIDEO_CAP_BUSY&&q.unavailable_reason==4);
 assert(!q.profile_mask&&!q.max_cpu_bytes&&!calls.alloc&&!calls.submits&&!calls.sleeps);
 IOLockUnlock(a.fUvdLock);a.videoQuery(true,&q);assert(q.flags==(RDN_VIDEO_CAP_READY|RDN_VIDEO_CAP_VALIDATION));
}
static void destructionFailure(int mode){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);auto d=destroy(s);
 if(mode==0)calls.failSubmit=2;if(mode==1)calls.failWait=2;if(mode==2)calls.idleError=-EIO;if(mode==3)calls.failGartFlush=2;
 if(mode==4)calls.failDMA=3;
 assert(a.videoDestroy(&owner,&d)==kIOReturnIOError);assert(a.fVideoSession&&a.fVideoSession->quarantined&&!calls.freed);
 if(mode<4)assert(!calls.gartFree&&calls.dmaClear==0);else assert(calls.gartFree==1&&calls.dmaClear==1);
 assert(a.fVideoSession->readbackDMA&&calls.dmaRelease==0);
}
static void staleToken(){
 RadeonNIAccel a;RadeonNIVideoUserClient first,second;auto s=create(a,first);auto d=picture(s);rdn_video_output out{};
 assert(a.videoDecode(&first,&d,&out)==0);auto stale=release(out);auto x=destroy(s);assert(a.videoDestroy(&first,&x)==0);
 auto next=create(a,second);assert(next.session.low!=s.session.low);int count=calls.submits;
 assert(a.videoRelease(&second,&stale)!=0&&a.videoDecode(&second,&d,&out)!=0&&calls.submits==count);
}

static void closeDuringWait(bool readback){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto s=create(a,owner);auto d=picture(s);rdn_video_output out{};
 if(readback)calls.busyCP=1;else calls.failWait=2;holdWait=true;int result=-1;std::atomic<bool> closed{false};
 std::thread decoder([&]{result=a.videoDecode(&owner,&d,&out);});
 {std::unique_lock<std::mutex> lock(waitMutex);waitChanged.wait(lock,[]{return waitEntered;});}
 IOLockLock(a.fLock);assert(!calls.freed);if(readback)assert(!calls.inflight&&!calls.cpInflight&&!a.fVideoSession->gfxPending);else assert(calls.inflight);IOLockUnlock(a.fLock);
 std::thread closer([&]{a.videoClose(&owner);closed=true;});
 assert(!closed.load()&&!calls.freed);
 {std::lock_guard<std::mutex> lock(waitMutex);calls.failWait=0;releaseWait=true;}
 waitChanged.notify_all();decoder.join();closer.join();assert(result==0&&closed&&calls.freed==3&&!a.fVideoSession);
}

static void minimumDimension(unsigned which){
 static const UInt32 dimensions[][2]={{16,64},{32,64},{48,64},{63,64},{64,16},{64,32},{64,48},{64,63},{32,32},{64,64},{64,128},{128,64},{320,192}};
 assert(which<sizeof(dimensions)/sizeof(dimensions[0]));
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session out{};
 c.config.coded_width=dimensions[which][0];c.config.coded_height=dimensions[which][1];
 auto result=a.videoCreate(&owner,true,&c,&out);
 if(which<9){
  assert(result==kIOReturnUnsupported&&!a.fVideoSession&&!a.fVideoEpoch&&!a.fVideoBusy);
  assert(!calls.alloc&&!calls.submits&&!a.fVideoCpuBytes&&!out.session.low);
 }else{
  assert(result==kIOReturnSuccess&&out.coded_width==c.config.coded_width&&out.coded_height==c.config.coded_height);
  assert(out.output_bytes==c.config.coded_width*c.config.coded_height*3/2);
  auto d=destroy(out);assert(a.videoDestroy(&owner,&d)==kIOReturnSuccess&&!a.fVideoSession&&!a.fVideoCpuBytes);
 }
}


static void feedbackLogging(unsigned which){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session s{};
 if(which==1){c.flags=0;a.fb.production=true;}
 assert(a.videoCreate(&owner,true,&c,&s)==0&&calls.feedbackLogs.empty());
 assert(calls.memoryLogs.size()==1&&calls.memoryLogs[0].find("session=00000000:00000001")!=std::string::npos);
 memset(a.fVideoSession->upload->getBytesNoCopy(),0x65,1408);
 if(which==2){
  auto d=picture(s);rdn_video_output out{};calls.failWait=2;
  assert(a.videoDecode(&owner,&d,&out)!=0&&calls.feedbackLogs.empty());return;
 }
 for(unsigned i=0;i<10;++i){
  auto d=picture(s);d.request_sequence=i+1;rdn_video_output out{};
  assert(a.videoDecode(&owner,&d,&out)==0);
  auto r=release(out);assert(a.videoRelease(&owner,&r)==0);
 }
 assert(calls.feedbackLogs.size()==(which==0?8u:0u));
 assert(calls.timingLogs.size()==(which==0?9u:0u));
 assert(calls.clockReads==(which==0?66u:0u));
 assert(calls.memoryLogs.size()==1);
 for(unsigned i=0;i<calls.feedbackLogs.size();++i){
  char expected[256];snprintf(expected,sizeof(expected),"RadeonNI UVD feedback session=00000000:00000001 request=%u raw=00000800 %08x 00000000 00000001 01020308 01020309 0102030a 0102030b\n",i+1,i+1);
  assert(calls.feedbackLogs[i]==expected);
 }
 auto x=destroy(s);assert(a.videoDestroy(&owner,&x)==0);
 assert(calls.feedbackLogs.size()==(which==0?8u:0u));
}
static void initializationFailure(unsigned mode){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session out{};
 if(mode==0)calls.busyInit=3;if(mode==1)calls.busyInit=2000;
 if(mode==2)calls.failInitSubmit=1;if(mode==3)calls.failInitWait=1;
 if(mode==4){calls.busyInit=1;calls.retireInit=true;}
 auto result=a.videoCreate(&owner,true,&c,&out);
 if(mode==0){assert(result==0&&calls.initSubmit==4&&calls.sleeps==3&&calls.submits==1);return;}
 assert(result!=0&&!out.session.low&&!calls.submits&&!calls.wake);
 if(mode==1||mode==4){assert(!a.fVideoSession&&!calls.cpInflight&&calls.freed==3&&!a.fUvd.failed&&!a.fVideoCpuBytes);}
 else{assert(a.fVideoSession&&a.fVideoSession->quarantined&&a.fVideoSession->gfxPending&&!calls.freed&&!calls.gartFree);}
}
static void feedbackFailure(unsigned mode){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session s{};
 if(mode==4){c.flags=0;a.fb.production=true;}
 assert(a.videoCreate(&owner,true,&c,&s)==0);
 memset(a.fVideoSession->upload->getBytesNoCopy(),0x65,1408);
 rdn_video_output prior{};auto d=picture(s);
 assert(a.videoDecode(&owner,&d,&prior)==0);
 unsigned request=2;
 if(mode==5){
  auto r=release(prior);assert(a.videoRelease(&owner,&r)==0);
  for(;request<9;++request){d.request_sequence=request;rdn_video_output o{};assert(a.videoDecode(&owner,&d,&o)==0);r=release(o);assert(a.videoRelease(&owner,&r)==0);}
 }
 calls.feedbackStatus=mode==0?1:mode==2?0xffffffffu:2;
 if(mode==3){calls.feedbackStatus=0;calls.staleFeedback=true;}
 int submits=calls.submits,copies=calls.cpSubmit;rdn_video_output out{};
 d.request_sequence=request;
 assert(a.videoDecode(&owner,&d,&out)==kIOReturnIOError&&!out.output_sequence);
 assert(calls.submits==submits+1&&calls.cpSubmit==copies&&!calls.inflight&&!calls.cpInflight);
 assert(a.fVideoSession->decodeFailed&&!a.fVideoSession->quarantined&&!a.fUvd.failed&&a.fUvd.ready);
 assert(a.videoDecode(&owner,&d,&out)==kIOReturnIOError&&calls.submits==submits+1);
 if(mode!=5){auto r=release(prior);assert(a.videoRelease(&owner,&r)==0);}
 auto x=destroy(s);assert(a.videoDestroy(&owner,&x)==0&&!a.fVideoSession&&calls.freed==3&&!a.fUvd.failed);
}
static void hiddenMemory(unsigned mode){
 RadeonNIAccel a;RadeonNIVideoUserClient owner;auto c=config();rdn_video_session out{};
 if(mode<2||mode>=4){c.config.coded_width=1920;c.config.coded_height=1088;c.config.level_idc=41;c.config.max_num_ref_frames=4;}
 uint64_t held=0;
 if(mode==0){assert(!actual_mem_alloc(&a.fMem,55*1024*1024,4096,&held));assert(rdn_mem_largest(&a.fMem)==5*1024*1024);}
 if(mode==1){rdn_mem_fini(&a.fHidden);assert(!rdn_mem_init(&a.fHidden,&heapOS,a.fHiddenOffset,4*1024*1024));}
 if(mode==2)a.fHiddenSize=0;
 if(mode==3)calls.allocFail=true;
 if(mode==4)assert(!actual_mem_alloc_in_range(&a.fHidden,32*1024*1024,4096,a.fHiddenOffset+78*1024*1024,a.fHiddenOffset+110*1024*1024,&held));
 if(mode==5){
  rdn_mem_fini(&a.fHidden);a.fHiddenOffset=0x1c000000;a.fHiddenSize=192*1024*1024;a.hidden.resize(a.fHiddenSize/4);
  assert(!rdn_mem_init(&a.fHidden,&heapOS,a.fHiddenOffset,a.fHiddenSize));
 }
 if(mode==6){
  assert(!actual_mem_alloc(&a.fMem,55*1024*1024,4096,&held));
  rdn_mem_fini(&a.fHidden);a.fHiddenSize=77*1024*1024;
  assert(!rdn_mem_init(&a.fHidden,&heapOS,a.fHiddenOffset,a.fHiddenSize));
 }
 auto result=a.videoCreate(&owner,true,&c,&out);
 if(mode==1||mode==3){
  assert(result==kIOReturnNoMemory&&!a.fVideoSession&&!calls.submits&&!calls.cpSubmit&&!a.fVideoCpuBytes);
  assert(calls.failureLogs.size()==1&&calls.failureLogs[0].find(mode==1?"stage=dpb-vram":"stage=visible-vram")!=std::string::npos);
  assert(rdn_mem_available(&a.fMem)==60*1024*1024);return;
 }
 if(mode==6){
  assert(result==kIOReturnNoMemory&&!a.fVideoSession&&calls.freed==2&&!calls.submits&&!calls.initSubmit&&!a.fVideoCpuBytes);
  assert(calls.failureLogs.size()==1&&calls.failureLogs[0].find("stage=target-vram")!=std::string::npos);
  assert(rdn_mem_available(&a.fMem)==5*1024*1024&&rdn_mem_available(&a.fHidden)==77*1024*1024);
  actual_mem_free(&a.fMem,held);return;
 }
 assert(result==0&&a.fVideoSession->gpuHidden[0]==(mode!=2)&&a.fVideoSession->gpuHidden[1]==(mode!=2));
 assert(calls.memoryLogs.size()==1&&calls.memoryLogs[0].find(mode==2?"hidden=0":"hidden=3")!=std::string::npos);
 if(mode==0){assert(a.fVideoSession->allocationBytes==4239360&&a.fVideoSession->gpuAllocationBytes[0]+a.fVideoSession->gpuAllocationBytes[1]==83525632);assert(a.fVideoCpuBytes==18046976);}
 if(mode==5){assert(a.fVideoSession->gpuAllocation[0]==0x20000000&&a.fVideoSession->gpuAllocation[1]==0x1c000000);}
 if(mode==5){assert(calls.memoryLogs[0].find("dpb=0x20000000/")!=std::string::npos&&calls.memoryLogs[0].find("target=0x1c000000/")!=std::string::npos);}
 auto d=picture(out);rdn_video_output frame{};assert(a.videoDecode(&owner,&d,&frame)==0&&frame.output_sequence==1);
 auto close=destroy(out);assert(a.videoDestroy(&owner,&close)==0&&calls.freed==3&&!a.fVideoSession);
 if(mode==0)actual_mem_free(&a.fMem,held);
 if(mode==4)actual_mem_free(&a.fHidden,held);
 assert(rdn_mem_available(&a.fMem)==60*1024*1024&&rdn_mem_available(&a.fHidden)==(mode==2?128*1024*1024:a.fHiddenSize));
}


static rdn_video_gpu_retire retireGpu(const rdn_video_gpu_output &out,UInt32 role,UInt32 outcome=0) {
 rdn_video_gpu_retire r{};r.version=1;r.bytes=sizeof(r);r.role=role;r.outcome=outcome;
 memcpy(r.source_token,out.source_token,sizeof(r.source_token));return r;
}
static void gpuLease(unsigned mode) {
 static_assert(sizeof(rdn_video_gpu_output)==64&&sizeof(rdn_video_gpu_acquire)==64&&sizeof(rdn_video_gpu_source)==96&&sizeof(rdn_video_gpu_retire)==64,"fixed GPU wire ABI");
 RadeonNIAccel a;RadeonNIVideoUserClient owner,other;a.fb.gpu=true;
 auto c=config();c.flags|=RDN_VIDEO_CREATE_GPU_OUTPUT;rdn_video_session session{};
 if(mode==12) {a.fb.gpu=false;assert(a.videoCreate(&owner,true,&c,&session)==kIOReturnUnsupported&&!calls.alloc);return;}
 assert(a.videoCreate(&owner,true,&c,&session)==0);
 assert(!session.output_slots&&!session.output0_memory_type&&!session.output1_memory_type);
 assert(!a.fVideoSession->output[0]&&!a.fVideoSession->output[1]);
 rdn_video_query q{};a.videoQuery(true,&q);assert(q.flags&RDN_VIDEO_CAP_GPU_OUTPUT);
 auto d=picture(session);rdn_video_gpu_output out{};
 if(mode==8)calls.corrupt=true;if(mode==9)calls.feedbackStatus=2;
 if(mode==13) {rdn_video_output wrong{};assert(a.videoDecode(&owner,&d,&wrong)==kIOReturnBadArgument&&calls.submits==1);}
 IOReturn decoded=a.videoDecodeGpu(&owner,&d,&out);
 if(mode==8||mode==9){assert(decoded==kIOReturnIOError&&videoZero(out.source_token,8));assert(a.fUvd.failed==(mode==8));return;}
 assert(decoded==0&&out.request_sequence==1&&out.coded_width==864&&out.coded_height==480&&out.layout==RDN_VIDEO_LAYOUT_FIELD_NV12);
 assert(calls.readBytes==12*4096); // Guard DMA only; no decoded-pixel readback.
 rdn_video_gpu_acquire request{};request.version=1;request.bytes=sizeof(request);memcpy(request.source_token,out.source_token,sizeof(request.source_token));
 rdn_video_gpu_source source{};auto producer=retireGpu(out,RDN_VIDEO_GPU_PRODUCER);auto importer=retireGpu(out,RDN_VIDEO_GPU_IMPORTER);
 if(mode==3){producer.outcome=RDN_VIDEO_GPU_PENDING;assert(a.videoRetireGpu(&owner,&producer)==0);a.videoClose(&owner);assert(a.fVideoSession&&a.fVideoBusy&&!calls.freed);return;}
 if(mode==6){a.videoClose(&owner);assert(!a.fVideoSession&&!a.fVideoBusy&&calls.freed==3);return;}
 if(mode==14){assert(a.videoRetireGpu(&owner,&producer)==0);assert(a.videoAcquireGpu(&owner,&request,&source)==kIOReturnBadArgument);return;}
 assert(a.videoAcquireGpu(&other,&request,&source)==kIOReturnBadArgument);
 request.source_token[2]++;assert(a.videoAcquireGpu(&owner,&request,&source)==kIOReturnBadArgument);request.source_token[2]--;
 if(mode==10){request.reserved[5]=1;assert(a.videoAcquireGpu(&owner,&request,&source)==kIOReturnBadArgument);request.reserved[5]=0;}
 assert(a.videoAcquireGpu(&owner,&request,&source)==0);
 assert(source.vram_offset==a.fVideoSession->addresses[kVideoTarget]-a.fAccel.vram_base&&source.target_bytes==a.fVideoSession->sizes[kVideoTarget]);
 assert(source.pitch==a.fVideoSession->layout.target_pitch&&source.y_bottom==a.fVideoSession->layout.luma_bottom_offset&&source.uv_bottom==a.fVideoSession->layout.chroma_bottom_offset);
 assert(a.videoAcquireGpu(&owner,&request,&source)==kIOReturnBadArgument);
 d.request_sequence=2;rdn_video_gpu_output next{};int submissions=calls.submits;
 assert(a.videoDecodeGpu(&owner,&d,&next)==kIOReturnBusy&&calls.submits==submissions);
 if(mode==4){importer.outcome=RDN_VIDEO_GPU_PENDING;assert(a.videoRetireGpu(&owner,&importer)==0);assert(a.videoRetireGpu(&owner,&producer)==0);a.videoClose(&owner);assert(a.fVideoSession&&!calls.freed);return;}
 if(mode==5){a.videoClose(&owner);assert(a.fVideoSession&&a.fVideoSession->quarantined&&a.fVideoBusy&&!calls.freed);return;}
 if(mode==7){assert(a.videoRetireGpu(&owner,&producer)==0);assert(a.videoDecodeGpu(&owner,&d,&next)==kIOReturnBusy);}
 if(mode==10){importer.role=3;assert(a.videoRetireGpu(&owner,&importer)==kIOReturnBadArgument);importer.role=2;importer.outcome=2;assert(a.videoRetireGpu(&owner,&importer)==kIOReturnBadArgument);importer.outcome=0;}
 assert(a.videoRetireGpu(&other,&importer)==kIOReturnBadArgument);
 assert(a.videoRetireGpu(&owner,&importer)==0);
 assert(a.videoRetireGpu(&owner,&importer)==kIOReturnBadArgument);
 if(mode!=7)assert(a.videoRetireGpu(&owner,&producer)==0);
 assert(a.videoAcquireGpu(&owner,&request,&source)==kIOReturnBadArgument);
 assert(a.videoDecodeGpu(&owner,&d,&next)==0&&memcmp(out.source_token,next.source_token,32));
 auto drop=retireGpu(next,RDN_VIDEO_GPU_PRODUCER);assert(a.videoRetireGpu(&owner,&drop)==0);
 if(mode==11){a.videoClose(&owner);rdn_video_session newer{};assert(a.videoCreate(&owner,true,&c,&newer)==0);assert(a.videoAcquireGpu(&owner,&request,&source)==kIOReturnBadArgument);}
}
static void broker(unsigned mode) {
 RadeonNIAccel a; RadeonNIVideoUserClient owner;
 rdn_video_create in=config(); rdn_video_session out{};
 bool claimed=false;
 if(mode==0 || mode==1) {
  if(mode==0)a.fUvdUsers=1;else a.fUvdLive=1;
  assert(a.videoCreate(&owner,true,&in,&out)==kIOReturnBusy);
  assert(!a.fVideoSession&&!a.fVideoBusy&&!calls.alloc&&!calls.submits);
  rdn_video_query q{};a.videoQuery(true,&q);assert(q.flags&RDN_VIDEO_CAP_BUSY);
  return;
 }
 if(mode==2) {
  a.fUvd.guarded=true;
  assert(a.uvdAcquireRaw(&claimed)==kIOReturnSuccess&&claimed&&a.fUvdUsers==1);
  UInt32 scratch=a.fUvdScratch;assert(scratch&&calls.alloc==1);
  assert(a.uvdAcquireRaw(&claimed)==kIOReturnSuccess&&a.fUvdUsers==1&&a.fUvdScratch==scratch&&calls.alloc==1);
  bool other=false;assert(a.uvdAcquireRaw(&other)==kIOReturnSuccess&&a.fUvdUsers==2&&calls.alloc==1);
  assert(a.videoCreate(&owner,true,&in,&out)==kIOReturnBusy&&!a.fVideoBusy);
  return;
 }
 if(mode==3) {
  assert(a.videoCreate(&owner,true,&in,&out)==kIOReturnSuccess);
  assert(a.uvdAcquireRaw(&claimed)==kIOReturnBusy&&!claimed&&!a.fUvdUsers);
  a.videoClose(&owner);assert(!a.fVideoBusy&&!a.fVideoSession);
  assert(a.uvdAcquireRaw(&claimed)==kIOReturnSuccess&&claimed&&a.fUvdUsers==1);
  return;
 }
 if(mode==4) {
  calls.metadataFail=true;assert(a.videoCreate(&owner,true,&in,&out)==kIOReturnNoMemory);
  assert(!a.fVideoBusy&&!a.fVideoSession);calls.metadataFail=false;
  assert(a.uvdAcquireRaw(&claimed)==kIOReturnSuccess&&claimed);return;
 }
 if(mode==5)a.fRetiring=true;
 if(mode==6)a.fUvd.failed=true;
 if(mode==7)a.fUvd.prepared=true;
 if(mode==8)a.fUvdUsers=UINT32_MAX;
 if(mode==9){a.fUvd.guarded=true;a.fUvd.ready=false;}
 if(mode==10){a.fUvd.guarded=true;calls.allocFail=true;}
 IOReturn wanted=mode==9?kIOReturnNotReady:mode==10?kIOReturnNoMemory:kIOReturnBusy;
 assert(a.uvdAcquireRaw(&claimed)==wanted&&!claimed&&!a.fUvdScratch);
 assert(a.fUvdUsers==(mode==8?UINT32_MAX:0));
}
int main(int argc,char**argv){assert(argc==2);std::string test=argv[1];if(test.rfind("gpu-lease",0)==0)gpuLease(atoi(argv[1]+9));else if(test.rfind("broker",0)==0)broker(atoi(argv[1]+6));else if(test.rfind("hidden",0)==0)hiddenMemory(atoi(argv[1]+6));else if(test.rfind("feedback-error",0)==0)feedbackFailure(atoi(argv[1]+14));else if(test.rfind("init-failure",0)==0)initializationFailure(atoi(argv[1]+12));else if(test.rfind("feedback",0)==0)feedbackLogging(atoi(argv[1]+8));else if(test.rfind("minimum",0)==0)minimumDimension(atoi(argv[1]+7));else if(test=="concurrent")closeDuringWait(false);else if(test=="cp-concurrent")closeDuringWait(true);else if(test=="cp-busy")readbackBusy();else if(test=="physical64")physical64();else if(test=="query-busy")queryBusy();else if(test.rfind("dma-failure",0)==0)dmaFailure(atoi(argv[1]+11));else if(test=="query")query();else if(test=="slots")slots();else if(test=="lifetime")lifetime();else if(test=="retired")retired();else if(test=="busy")busy();else if(test=="quota")quota();else if(test=="stale")staleToken();else if(test.rfind("creation",0)==0)creationFailure(atoi(argv[1]+8));else if(test.rfind("readback",0)==0)readbackFailure(atoi(argv[1]+8));else if(test.rfind("destruction",0)==0)destructionFailure(atoi(argv[1]+11));else if(test.rfind("error",0)==0)errors(atoi(argv[1]+5));else if(test.rfind("invalid",0)==0)invalid(atoi(argv[1]+7));else assert(false);return 0;}
"""
class SessionTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();tmp=pathlib.Path(cls.tmp.name)
  header=(ROOT/'kext/RadeonNI/RadeonNIVideoMemory.h').read_text();header=header[header.index('class RadeonNIVideoMemory'):header.rindex('#endif')]
  memory=(ROOT/'kext/RadeonNI/RadeonNIVideoMemory.cpp').read_text();memory=memory[memory.index('bool RadeonNIAccel::videoCharge'):]
  session=(ROOT/'kext/RadeonNI/RadeonNIUvdSession.cpp').read_text();session=session[session.index('enum { kVideoPage'):]
  accel=(ROOT/'kext/RadeonNI/RadeonNIAccel.cpp').read_text()
  raw=accel[accel.index('IOReturn RadeonNIAccel::uvdAcquireRaw('):accel.index('IOReturn RadeonNIUserClient::claimUvd(')]
  (tmp/'test.cpp').write_text(PREFIX+raw+header+'\n#define super IOGeneralMemoryDescriptor\n'+memory+'\n#undef super\n'+session+SUFFIX)
  cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));flags=shlex.split(os.environ.get('TEST_CXXFLAGS','-O2'))
  subprocess.run(cc+flags+['-std=c99','-I'+str(ROOT/'hw'),'-c',str(ROOT/'hw/rdn_uvd_h264.c'),'-o',str(tmp/'pack.o')],check=True)
  subprocess.run(cc+flags+['-std=c99','-I'+str(ROOT/'hw'),'-c',str(ROOT/'hw/rdn_cp_dma_readback.c'),'-o',str(tmp/'readback.o')],check=True)
  subprocess.run(cc+flags+['-std=c99','-Drdn_mem_alloc=actual_mem_alloc','-Drdn_mem_alloc_in_range=actual_mem_alloc_in_range','-Drdn_mem_free=actual_mem_free','-I'+str(ROOT/'hw'),'-c',str(ROOT/'hw/rdn_mem.c'),'-o',str(tmp/'mem.o')],check=True)
  cls.exe=tmp/'test';subprocess.run(cxx+flags+['-std=c++17','-pthread','-I'+str(ROOT/'hw'),str(tmp/'test.cpp'),str(tmp/'pack.o'),str(tmp/'readback.o'),str(tmp/'mem.o'),'-o',str(cls.exe)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def run_case(self,name):subprocess.run(shlex.split(os.environ.get('TEST_RUNNER',''))+[str(self.exe),name],check=True,timeout=30)
 def test_gfx_progress_and_close_during_wait(self):self.run_case("concurrent")
 def test_gfx_progress_and_close_during_cp_retry(self):self.run_case("cp-concurrent")
 def test_cp_busy_retries_without_redecode(self):self.run_case("cp-busy")
 def test_physical_pages_above_4gib(self):self.run_case("physical64")
 def test_dma_mapping_preflight_and_segments(self):
  for i in range(6):self.run_case('dma-failure'+str(i))
 def test_split_hidden_memory_and_cleanup(self):
  for i in range(7):self.run_case('hidden'+str(i))
 def test_gpu_output_lease_and_guard_only_dma(self):
  for i in range(15):self.run_case('gpu-lease'+str(i))
 def test_shared_raw_typed_engine_admission(self):
  for i in range(11):self.run_case('broker'+str(i))
 def test_query(self):self.run_case('query')
 def test_query_does_not_wait_for_decode_lock(self):self.run_case('query-busy')
 def test_slot_ownership_and_backpressure(self):self.run_case('slots')
 def test_stale_map_charge_and_readonly(self):self.run_case('lifetime')
 def test_retirement_before_publish(self):self.run_case('retired')
 def test_busy_is_retryable(self):self.run_case('busy')
 def test_global_stale_mapping_quota(self):self.run_case('quota')
 def test_quarantine(self):
  for i in range(3):self.run_case('error'+str(i))
 def test_create_failure_lifetimes(self):
  for i in range(5):self.run_case("creation"+str(i))
 def test_readback_dma_failure_lifetimes(self):
  for i in range(5):self.run_case("readback"+str(i))
 def test_destroy_failure_lifetimes(self):
  for i in range(5):self.run_case("destruction"+str(i))
 def test_stale_tokens_after_recreate(self):self.run_case("stale")
 def test_initialization_dma_lifetimes(self):
  for i in range(5):self.run_case("init-failure"+str(i))
 def test_completed_feedback_errors_do_not_publish_or_poison(self):
  for i in range(6):self.run_case("feedback-error"+str(i))
 def test_validation_feedback_logging(self):
  for i in range(3):self.run_case("feedback"+str(i))
 def test_minimum_coded_dimensions(self):
  for i in range(13):self.run_case("minimum"+str(i))
 def test_validation_before_allocation(self):
  for i in range(5):self.run_case('invalid'+str(i))
if __name__=='__main__':unittest.main()
