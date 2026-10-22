/* One bounded UVD stream; client mappings never contain GPU commands. */
#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include "RadeonNI.h"
#include "RadeonNIVideo.h"
#include "RadeonNIVideoMemory.h"
extern "C" {
#include "rdn_cp_dma_readback.h"
}

enum { kVideoPage=4096, kVideoTimeout=1000, kVideoSpans=6 };
enum { kVideoMessage,kVideoFeedback,kVideoIB,kVideoBitstream,kVideoDPB,kVideoTarget };
struct RadeonNIVideoSession {
 RadeonNIVideoUserClient *owner;
 rdn_video_token token;
 rdn_uvd_h264_config config;
 rdn_uvd_h264_layout layout;
 rdn_uvd_h264_ib_buffers buffers;
 RadeonNIVideoMemory *upload, *output[2], *readback;
 IODMACommand *readbackDMA;
 UInt32 gartOffset, readbackBytes;
 bool readbackPrepared, gartAllocated, gartUnsafe, gfxPending;
 UInt8 *snapshot;
 UInt32 offsets[kVideoSpans], sizes[kVideoSpans], allocation, allocationBytes;
 UInt32 nextRequest, nextOutput, held[2], handle;
 bool allocated, submitted, created, destroyed, quarantined, snapshotCharged, awake, validation, decodeFailed;
 UInt64 allocatedAddress, gpuAllocation[2], addresses[kVideoSpans];
 UInt32 gpuAllocationBytes[2];
 bool gpuAllocated[2], gpuHidden[2];
 UInt8 message[RDN_UVD_H264_MESSAGE_BYTES], ib[4096];
};
static UInt64 videoTimeUs(bool enabled)
{
 if(!enabled) return 0;
 UInt64 ticks,nanos;
 clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks,&nanos);
 return nanos/1000;
}
static bool videoZero(const uint32_t *words,unsigned count)
{ for(unsigned i=0;i<count;++i) if(words[i]) return false; return true; }
static bool videoToken(const rdn_video_token &a,const rdn_video_token &b)
{ return a.high==b.high && a.low==b.low && (a.high||a.low); }
static void videoWrite(volatile UInt8 *out,const void *in,UInt32 bytes)
{ memcpy((void *)out,in,bytes); }
static void videoFill(volatile UInt8 *out,UInt32 bytes,UInt32 value)
{ volatile UInt32 *p=(volatile UInt32 *)out; for(UInt32 i=0;i<bytes/4;++i) p[i]=value; }
static UInt32 videoProfile(UInt32 profile)
{ return profile==66?RDN_VIDEO_PROFILE_BASELINE:profile==77?RDN_VIDEO_PROFILE_MAIN:profile==100?RDN_VIDEO_PROFILE_HIGH:0; }
static bool videoGuards(const RadeonNIVideoSession *s)
{
 const UInt32 *guard=(const UInt32 *)((const UInt8 *)s->readback->getBytesNoCopy()+s->layout.target_allocation_bytes);
 for(UInt32 i=0;i<2*kVideoSpans*kVideoPage/4;++i) if(guard[i]!=0xa5a5a5a5u) return false;
 return true;
}
// UVD objects must not straddle a 256MiB GPU segment.
static int videoAllocateSegment(rdn_mem *pool,UInt64 gpuBase,UInt32 bytes,
 UInt64 minimum,UInt64 maximum,UInt64 *out)
{
 const UInt64 segment=1ull<<28,limit=1ull<<40;
 if(!bytes || bytes>segment || minimum>=maximum || gpuBase>=limit || maximum>limit-gpuBase)
  return -EINVAL;
 for(UInt64 at=minimum;at<maximum;) {
  UInt64 end=(((gpuBase+at)/segment)+1)*segment-gpuBase;
  if(end>maximum) end=maximum;
  if(bytes<=end-at && !rdn_mem_alloc_in_range(pool,bytes,kVideoPage,at,end,out)) return 0;
  at=end;
 }
 return -ENOMEM;
}
/* fUvdLock held; brief fLock sections never enclose a firmware wait. */
bool RadeonNIAccel::videoReady(bool administrator,bool validation)
{
 if(fRetiring || !fLock) return false;
 IOLockLock(fLock);
 bool ready=fEngineUp && fGart.ready && fFramebuffer && fUvdProvider && fUvd.ready && !fUvd.failed && !fUvd.stopped &&
  fUvdState==RDN_UVD_DIAGNOSTIC_RING_READY && fUvdProvider->getProperty(RDN_UVD_REBOOT_PROPERTY)==kOSBooleanTrue;
 if(ready && validation) ready=administrator && fFramebuffer->getProperty("EnableUVDValidation")==kOSBooleanTrue;
 else if(ready) {
  OSNumber *version=OSDynamicCast(OSNumber,fFramebuffer->getProperty("UVDValidatedAPI"));
  ready=fFramebuffer->getProperty("EnableUVDSessions")==kOSBooleanTrue && version && version->unsigned32BitValue()==RDN_VIDEO_API_VERSION;
 }
 IOLockUnlock(fLock);
 return ready;
}
void RadeonNIAccel::videoQuery(bool administrator,rdn_video_query *out)
{
 bzero(out,sizeof(*out)); out->version=RDN_VIDEO_API_VERSION; out->bytes=sizeof(*out);
 if(!IOLockTryLock(fUvdLock)) {
  out->flags=RDN_VIDEO_CAP_BUSY; out->unavailable_reason=4; return;
 }
 bool production=videoReady(false,false),validation=videoReady(administrator,true);
 if(production||validation) out->flags|=RDN_VIDEO_CAP_READY;
 if(production) out->flags|=RDN_VIDEO_CAP_PRODUCTION;
 if(validation) out->flags|=RDN_VIDEO_CAP_VALIDATION;
 if(fVideoSession) out->flags|=RDN_VIDEO_CAP_BUSY;
 out->unavailable_reason=fRetiring?1:fUvd.failed?2:!(production||validation)?3:0;
 if(production||validation) {
  out->max_width=RDN_UVD_H264_MAX_WIDTH; out->max_height=RDN_UVD_H264_MAX_HEIGHT;
  out->max_refs=RDN_UVD_H264_MAX_REFS;
  out->profile_mask=RDN_VIDEO_PROFILE_BASELINE|RDN_VIDEO_PROFILE_MAIN|RDN_VIDEO_PROFILE_HIGH;
  out->level_mask=0x1fff; /* Levels10,11,12,13,20,21,22,30,31,32,40,41,42. */
  out->max_upload_bytes=RDN_VIDEO_MAX_UPLOAD_BYTES; out->max_sessions=1; out->max_inflight=1;
  out->output_slots=2; out->max_vram_bytes=128u*1024*1024; out->max_cpu_bytes=20u*1024*1024;
 }
 IOLockUnlock(fUvdLock);
}
void RadeonNIAccel::videoPoison(int error)
{
 fUvd.failed=true; fUvd.ready=false;
 fUvdState=RDN_UVD_DIAGNOSTIC_FAULTED; fUvdError=error;
 if(fVideoSession) fVideoSession->quarantined=true;
}
int RadeonNIAccel::videoCommand(UInt32 type,const rdn_uvd_h264_picture *picture,UInt32 bitstreamBytes)
{
 RadeonNIVideoSession *s=fVideoSession;
 rdn_uvd_h264_ib_buffers buffers=s->buffers;
 int error; uint32_t words=0,sequence=0;
 if(type==RDN_UVD_H264_MSG_CREATE) error=rdn_uvd_h264_pack_create(&s->config,&s->layout,s->handle,s->message,sizeof(s->message));
 else if(type==RDN_UVD_H264_MSG_DECODE) error=rdn_uvd_h264_pack_decode(&s->config,picture,&s->layout,s->handle,s->nextRequest,bitstreamBytes,s->message,sizeof(s->message));
 else error=rdn_uvd_h264_pack_destroy(s->handle,s->message,sizeof(s->message));
 if(error) return -EINVAL;
 if(type!=RDN_UVD_H264_MSG_DECODE) {
  bzero(&buffers.dpb,sizeof(buffers.dpb)); bzero(&buffers.bitstream,sizeof(buffers.bitstream));
  bzero(&buffers.target,sizeof(buffers.target)); bzero(&buffers.feedback,sizeof(buffers.feedback));
 }
 if(rdn_uvd_h264_build_ib(&s->config,&s->layout,type,fUvd.swapped?RDN_UVD_H264_IB_BE:RDN_UVD_H264_IB_LE,
    bitstreamBytes,&buffers,s->ib,sizeof(s->ib),&words)) return -EINVAL;
 volatile UInt8 *base=(volatile UInt8 *)fAccel.aperture+s->allocation;
 videoWrite(base+s->offsets[kVideoMessage],s->message,sizeof(s->message));
 videoWrite(base+s->offsets[kVideoIB],s->ib,words*4);
 /* HDP flush is a single direct write of1, also issued by the independent GFX ring. */
 rdn_hdp_flush(&fAccel);
 IOLockLock(fLock);
 if(fRetiring) error=-ECANCELED;
 else {
  error=rdn_uvd_ib_try_submit(&fUvd,buffers.ib.address,words,&sequence);
  if(error!=-EBUSY) s->submitted=true;
 }
 IOLockUnlock(fLock);
 if(error==-EBUSY || error==-ECANCELED) return error;
 if(error) { videoPoison(error); return error; }
 bool complete=false;
 for(unsigned waited=0;waited<kVideoTimeout;++waited) {
  IOLockLock(fLock); complete=rdn_uvd_fence_done(&fUvd,sequence); IOLockUnlock(fLock);
  if(complete) break;
  IOSleep(1);
 }
 if(!complete) { IOLockLock(fLock); complete=rdn_uvd_fence_done(&fUvd,sequence); IOLockUnlock(fLock); }
 if(!complete) { videoPoison(-ETIMEDOUT); return -ETIMEDOUT; }
 return 0;
}
int RadeonNIAccel::videoBindReadback(void)
{
 RadeonNIVideoSession *s=fVideoSession;
 if(s->readback->prepare()!=kIOReturnSuccess) return -EIO;
 s->readbackPrepared=true;
 // Keep DART bus addresses; getPhysicalSegment64 returns unmapped CPU pages.
 s->readbackDMA=IODMACommand::withSpecification(IODMACommand::OutputHost64,40,
  RDN_GART_PAGE_SIZE,(IODMACommand::MappingOptions)(IODMACommand::kMapped|IODMACommand::kIterateOnly),
  s->readbackBytes,RDN_GART_PAGE_SIZE);
 if(!s->readbackDMA || s->readbackDMA->setMemoryDescriptor(s->readback)!=kIOReturnSuccess) return -EIO;
 uint64_t at=0;
 IOLockLock(fLock);
 if(fRetiring || !fGart.ready || rdn_mem_alloc(&fGartSpace,s->readbackBytes,RDN_GART_PAGE_SIZE,&at)) {
  IOLockUnlock(fLock); return -ENOMEM;
 }
 if(at<RDN_GART_PAGE_SIZE || at>(uint64_t)RDN_GART_PAGES*RDN_GART_PAGE_SIZE ||
    s->readbackBytes>(uint64_t)RDN_GART_PAGES*RDN_GART_PAGE_SIZE-at || (at&(RDN_GART_PAGE_SIZE-1))) {
  rdn_mem_free(&fGartSpace,at); IOLockUnlock(fLock); return -EINVAL;
 }
 s->gartOffset=(UInt32)at; s->gartAllocated=true;
 UInt32 done=0;
 for(;done<s->readbackBytes;done+=RDN_GART_PAGE_SIZE) {
  UInt64 offset=done; UInt32 count=1;
  IODMACommand::Segment64 segment;
  bzero(&segment,sizeof(segment));
  IOReturn result=s->readbackDMA->gen64IOVMSegments(&offset,&segment,&count);
  if(result!=kIOReturnSuccess || count!=1 || offset!=(UInt64)done+RDN_GART_PAGE_SIZE ||
     segment.fLength!=RDN_GART_PAGE_SIZE || !segment.fIOVMAddr ||
     segment.fIOVMAddr>(1ull<<40)-RDN_GART_PAGE_SIZE ||
     (segment.fIOVMAddr&(RDN_GART_PAGE_SIZE-1))) break;
  rdn_gart_set_page(&fGart,(s->gartOffset+done)/RDN_GART_PAGE_SIZE,segment.fIOVMAddr);
 }
 int error=done==s->readbackBytes?rdn_gart_flush_checked(&fGart):-EINVAL;
 if(error && done==s->readbackBytes) { s->gartUnsafe=true; videoPoison(error); }
 IOLockUnlock(fLock);
 return error;
}
int RadeonNIAccel::videoUnbindReadback(void)
{
 RadeonNIVideoSession *s=fVideoSession;
 if(s->gartAllocated) {
  IOLockLock(fLock);
  for(UInt32 at=0;at<s->readbackBytes;at+=RDN_GART_PAGE_SIZE)
   rdn_gart_clear_page(&fGart,(s->gartOffset+at)/RDN_GART_PAGE_SIZE);
  int error=rdn_gart_flush_checked(&fGart);
  if(error) s->gartUnsafe=true;
  else { rdn_mem_free(&fGartSpace,s->gartOffset); s->gartAllocated=false; }
  IOLockUnlock(fLock);
  if(error) return error;
 }
 if(s->readbackDMA) {
  if(s->readbackDMA->clearMemoryDescriptor()!=kIOReturnSuccess) { s->gartUnsafe=true; return -EIO; }
  s->readbackDMA->release(); s->readbackDMA=0;
 }
 if(s->readbackPrepared) {
  if(s->readback->complete()!=kIOReturnSuccess) { s->gartUnsafe=true; return -EIO; }
  s->readbackPrepared=false;
 }
 return 0;
}
int RadeonNIAccel::videoReadback(void)
{
 RadeonNIVideoSession *s=fVideoSession;
 struct rdn_cp_dma_copy copies[1+2*kVideoSpans];
 bzero(copies,sizeof(copies));
 uint64_t destination=RDN_GART_GPU_START+s->gartOffset;
 copies[0].source.address=s->buffers.target.address;
 copies[0].source.bytes=copies[0].destination.bytes=copies[0].bytes=s->layout.target_allocation_bytes;
 copies[0].destination.address=destination;
 for(unsigned i=0;i<kVideoSpans;++i) for(unsigned after=0;after<2;++after) {
  unsigned index=1+2*i+after;
  copies[index].source.address=s->addresses[i];
  if(after) copies[index].source.address+=s->sizes[i];
  else copies[index].source.address-=kVideoPage;
  copies[index].destination.address=destination+s->layout.target_allocation_bytes+(2*i+after)*kVideoPage;
  copies[index].source.bytes=copies[index].destination.bytes=copies[index].bytes=kVideoPage;
 }
 uint32_t words=0;
 if(rdn_cp_dma_readback_build_list(copies,1+2*kVideoSpans,s->ib,sizeof(s->ib),&words)) { videoPoison(-EINVAL); return -EINVAL; }
 return videoSubmitGfx(words,true);
}
int RadeonNIAccel::videoSubmitGfx(UInt32 words,bool poisonOnBusy)
{
 RadeonNIVideoSession *s=fVideoSession;
 uint32_t fence=0;
 videoWrite((volatile UInt8 *)fAccel.aperture+s->allocation+s->offsets[kVideoIB],s->ib,words*4);
 int error; unsigned waited=0;
 for(;;) {
  IOLockLock(fLock);
  if(fRetiring) error=-ECANCELED;
  else {
   error=rdn_ib_try_submit(&fAccel,s->buffers.ib.address,words,&fence);
   if(error!=-EBUSY) s->gfxPending=true;
  }
  IOLockUnlock(fLock);
  if(error!=-EBUSY) break;
  if(waited==kVideoTimeout) { error=-ETIMEDOUT; break; }
  IOSleep(1); ++waited;
 }
 if(error) { if(poisonOnBusy || s->gfxPending) videoPoison(error); return error; }
 bool complete=false;
 for(;waited<kVideoTimeout;++waited) {
  IOLockLock(fLock); complete=rdn_fence_done(&fAccel,fence); IOLockUnlock(fLock);
  if(complete) break;
  IOSleep(1);
 }
 if(!complete) { IOLockLock(fLock); complete=rdn_fence_done(&fAccel,fence); IOLockUnlock(fLock); }
 if(!complete) { videoPoison(-ETIMEDOUT); return -ETIMEDOUT; }
 s->gfxPending=false;
 OSSynchronizeIO();
 return 0;
}
int RadeonNIAccel::videoInitializeVram(void)
{
 RadeonNIVideoSession *s=fVideoSession;
 rdn_cp_dma_fill fills[3*kVideoSpans-1];
 bzero(fills,sizeof(fills));
 uint32_t count=0,words=0;

 for(unsigned i=0;i<kVideoSpans;++i) {
  if(i!=kVideoIB) {
   fills[count].destination.address=s->addresses[i];
   fills[count++].destination.bytes=s->sizes[i];
  }
  for(unsigned after=0;after<2;++after) {
   fills[count].destination.address=s->addresses[i];
   if(after) fills[count].destination.address+=s->sizes[i];
   else fills[count].destination.address-=kVideoPage;
   fills[count].destination.bytes=kVideoPage;
   fills[count++].value=0xa5a5a5a5u;
  }
 }
 rdn_cp_dma_span ib={s->buffers.ib.address,s->buffers.ib.bytes,0};
 if(rdn_cp_dma_fill_build_list(fills,count,&ib,s->ib,sizeof(s->ib),&words)) return -EINVAL;
 // The executing IB must never be a CP_DMA fill destination.
 videoFill((volatile UInt8 *)fAccel.aperture+s->allocation+s->offsets[kVideoIB],kVideoPage,0);
 return videoSubmitGfx(words,false);
}
void RadeonNIAccel::videoCleanup(bool destroy)
{
 RadeonNIVideoSession *s=fVideoSession;
 if(!s) return;
 if(destroy && s->created && !s->quarantined && !fUvd.failed) {
  int error=videoCommand(RDN_UVD_H264_MSG_DESTROY,0,0);
  if(!error) s->destroyed=true;
  if(error) videoPoison(error);
 }
 if(s->awake && (!s->submitted || s->destroyed) && !s->quarantined && !fUvd.failed) {
  int error=rdn_uvd_idle(&fUvd);
  if(error) videoPoison(error);
  else s->awake=false;
 }
 bool safe=(!s->submitted || (s->destroyed && !s->quarantined && !fUvd.failed)) && !s->gartUnsafe && !s->gfxPending;
 if(safe && videoUnbindReadback()) { videoPoison(-EIO); safe=false; }
 if(safe && s->readback) { s->readback->release(); s->readback=0; }
 if(s->upload) { s->upload->release(); s->upload=0; }
 for(unsigned i=0;i<2;++i) if(s->output[i]) { s->output[i]->release(); s->output[i]=0; }
 if(s->snapshot) { IOFree(s->snapshot,RDN_VIDEO_MAX_UPLOAD_BYTES); s->snapshot=0; }
 if(s->snapshotCharged) { videoUncharge(RDN_VIDEO_MAX_UPLOAD_BYTES); s->snapshotCharged=false; }
 s->owner=0;
 if(safe) {
  IOLockLock(fLock);
  for(unsigned i=0;i<2;++i)
   if(s->gpuAllocated[i]) rdn_mem_free(s->gpuHidden[i]?&fHidden:&fMem,s->gpuAllocation[i]);
  if(s->allocated) rdn_mem_free(&fMem,s->allocatedAddress);
  fVideoBusy=false;
  IOLockUnlock(fLock);
  IOFree(s,sizeof(*s)); fVideoSession=0;
 } else {
  s->quarantined=true;
  /* Keep the single quota occupied until reboot; DMA ownership is uncertain. */
 }
}
IOReturn RadeonNIAccel::videoCreate(RadeonNIVideoUserClient *owner,bool administrator,const rdn_video_create *in,rdn_video_session *out)
{
 if(in->version!=RDN_VIDEO_API_VERSION || in->bytes!=sizeof(*in) ||
    (in->flags&~RDN_VIDEO_CREATE_VALIDATION) || !videoZero(in->reserved,7)) return kIOReturnBadArgument;
 // Smaller coded frames completed with unchanged output on Turks.
 if(in->config.coded_width<64 || in->config.coded_height<64) return kIOReturnUnsupported;
 rdn_uvd_h264_layout layout;
 if(!videoProfile(in->config.profile_idc) || rdn_uvd_h264_layout_init(&in->config,RDN_UVD_H264_TARGET_FIELDS,&layout)) return kIOReturnUnsupported;
 IOLockLock(fUvdLock);
 IOReturn result=kIOReturnNoMemory;
 RadeonNIVideoSession *s=0;
 uint64_t allocation=0,address=0; uint32_t words=0;
 UInt64 visibleFree=0,visibleLargest=0,hiddenFree=0,hiddenLargest=0,segmentStart=0,segmentEnd=0;
 UInt32 cpuBytes=0; const char *failureStage="session";
 if(!videoReady(administrator,(in->flags&RDN_VIDEO_CREATE_VALIDATION)!=0)) { result=kIOReturnNotReady; goto done; }
 if(fVideoSession) { result=kIOReturnBusy; goto done; }
 if(fVideoEpoch==UINT64_MAX) { result=kIOReturnNoResources; goto done; }
 s=(RadeonNIVideoSession *)IOMalloc(sizeof(*s));
 if(!s) goto fail;
 bzero(s,sizeof(*s)); fVideoSession=s; s->owner=owner; s->config=in->config; s->layout=layout;
 ++fVideoEpoch; s->token.high=(UInt32)(fVideoEpoch>>32); s->token.low=(UInt32)fVideoEpoch;
 s->validation=(in->flags&RDN_VIDEO_CREATE_VALIDATION)!=0;
 s->handle=1; s->nextRequest=s->nextOutput=1;
 s->sizes[kVideoMessage]=s->sizes[kVideoFeedback]=s->sizes[kVideoIB]=kVideoPage;
 s->sizes[kVideoBitstream]=RDN_VIDEO_MAX_UPLOAD_BYTES;
 s->sizes[kVideoDPB]=layout.dpb_allocation_bytes; s->sizes[kVideoTarget]=layout.target_allocation_bytes;
 for(unsigned i=0;i<kVideoSpans;++i) {
  UInt32 *bytes=i<kVideoDPB?&s->allocationBytes:&s->gpuAllocationBytes[i-kVideoDPB];
  if(s->sizes[i] > 128u*1024*1024-s->allocationBytes-s->gpuAllocationBytes[0]-s->gpuAllocationBytes[1]-2*kVideoPage) { result=kIOReturnNoResources; goto fail; }
  s->offsets[i]=*bytes+kVideoPage; *bytes+=s->sizes[i]+2*kVideoPage;
 }
 if(2ull*RDN_VIDEO_MAX_UPLOAD_BYTES+2ull*layout.packed_nv12_bytes>20u*1024*1024) { result=kIOReturnNoResources; goto fail; }
 // Only message/feedback/IB/bitstream require CPU aperture access.
 failureStage="visible-vram";
 if(!fAccel.aperture || fAccel.vram_base>=(1ull<<40) ||
    fAccel.aperture_size>(1ull<<40)-fAccel.vram_base) { result=kIOReturnBadArgument; goto fail; }
 segmentStart=fUvd.vcpu_gpu&~((1ull<<28)-1); segmentEnd=segmentStart+(1ull<<28);
 if(segmentEnd<=fAccel.vram_base || segmentStart>=fAccel.vram_base+fAccel.aperture_size) { result=kIOReturnBadArgument; goto fail; }
 segmentStart=segmentStart>fAccel.vram_base?segmentStart-fAccel.vram_base:0;
 segmentEnd-=fAccel.vram_base;
 if(segmentEnd>fAccel.aperture_size) segmentEnd=fAccel.aperture_size;
 IOLockLock(fLock);
 fVideoBusy=true;
 if(!videoAllocateSegment(&fMem,fAccel.vram_base,s->allocationBytes,segmentStart,segmentEnd,&allocation)) {
  s->allocated=true; s->allocatedAddress=allocation;
 }
 IOLockUnlock(fLock);
 if(!s->allocated) goto fail;
 for(unsigned i=0;i<2;++i) {
  failureStage=i?"target-vram":"dpb-vram";
  IOLockLock(fLock);
  if(fHiddenSize && !videoAllocateSegment(&fHidden,fAccel.vram_base,s->gpuAllocationBytes[i],
     fHiddenOffset,(UInt64)fHiddenOffset+fHiddenSize,&s->gpuAllocation[i])) {
   s->gpuAllocated[i]=true; s->gpuHidden[i]=true;
  } else if(!videoAllocateSegment(&fMem,fAccel.vram_base,s->gpuAllocationBytes[i],
     0,fAccel.aperture_size,&s->gpuAllocation[i])) s->gpuAllocated[i]=true;
  IOLockUnlock(fLock);
  if(!s->gpuAllocated[i]) goto fail;
 }
 if(allocation>fAccel.aperture_size || s->allocationBytes>fAccel.aperture_size-allocation ||
    !fAccel.aperture || (uintptr_t)fAccel.aperture>UINTPTR_MAX-fAccel.aperture_size ||
    fAccel.vram_base>=(1ull<<40) || allocation>=(1ull<<40)-fAccel.vram_base) { result=kIOReturnBadArgument; goto fail; }
 s->allocation=(UInt32)allocation; address=fAccel.vram_base+allocation;
 if(s->allocationBytes>(1ull<<40)-address || fUvd.ring_words>RDN_UVD_MAX_RING_BYTES/4) { result=kIOReturnBadArgument; goto fail; }
 s->buffers.vcpu.address=fUvd.vcpu_gpu; s->buffers.vcpu.bytes=fUvd.vcpu_bytes;
 s->buffers.ring.address=fUvd.ring_gpu; s->buffers.ring.bytes=fUvd.ring_words*4+RDN_UVD_RING_EXTRA;
 for(unsigned i=0;i<kVideoSpans;++i)
  s->addresses[i]=fAccel.vram_base+(i<kVideoDPB?allocation:s->gpuAllocation[i-kVideoDPB])+s->offsets[i];
 #define VIDEO_SPAN(name,index) s->buffers.name.address=s->addresses[index]; s->buffers.name.bytes=s->sizes[index]
 VIDEO_SPAN(message,kVideoMessage); VIDEO_SPAN(feedback,kVideoFeedback); VIDEO_SPAN(ib,kVideoIB);
 VIDEO_SPAN(bitstream,kVideoBitstream); VIDEO_SPAN(dpb,kVideoDPB); VIDEO_SPAN(target,kVideoTarget);
 #undef VIDEO_SPAN
 if((address<s->buffers.vcpu.address+s->buffers.vcpu.bytes && s->buffers.vcpu.address<address+s->allocationBytes) ||
    (address<s->buffers.ring.address+s->buffers.ring.bytes && s->buffers.ring.address<address+s->allocationBytes) ||
    rdn_uvd_h264_build_ib(&s->config,&s->layout,RDN_UVD_H264_MSG_DECODE,
     fUvd.swapped?RDN_UVD_H264_IB_BE:RDN_UVD_H264_IB_LE,128,&s->buffers,s->ib,sizeof(s->ib),&words)) { result=kIOReturnBadArgument; goto fail; }
 failureStage="readback-ram";
 s->readbackBytes=layout.target_allocation_bytes+2*kVideoSpans*kVideoPage;
 s->readback=RadeonNIVideoMemory::withBytes(this,s->readbackBytes,true);
 if(!s->readback) goto fail;
 failureStage="upload-ram";
 s->upload=RadeonNIVideoMemory::withBytes(this,RDN_VIDEO_MAX_UPLOAD_BYTES,false);
 if(!s->upload) goto fail;
 failureStage="snapshot-quota";
 s->snapshotCharged=videoCharge(RDN_VIDEO_MAX_UPLOAD_BYTES);
 if(!s->snapshotCharged) goto fail;
 failureStage="snapshot-ram"; s->snapshot=(UInt8 *)IOMalloc(RDN_VIDEO_MAX_UPLOAD_BYTES);
 if(!s->snapshot) goto fail;
 for(unsigned i=0;i<2;++i) {
  failureStage=i?"output1-ram":"output0-ram";
  s->output[i]=RadeonNIVideoMemory::withBytes(this,layout.packed_nv12_bytes,true);
  if(!s->output[i]) goto fail;
 }
 failureStage="gart";
 if(videoBindReadback()) { result=kIOReturnVMError; goto fail; }
 failureStage="vram-init";
 bzero(s->upload->getBytesNoCopy(),s->upload->getLength());
 for(unsigned i=0;i<2;++i) bzero(s->output[i]->getBytesNoCopy(),s->output[i]->getLength());
 {
  UInt64 start=videoTimeUs(s->validation);
  int error=videoInitializeVram();
  UInt64 initialized=videoTimeUs(s->validation);
  if(s->validation) IOLog("RadeonNI UVD create timing session=%08x:%08x bytes=%u dpb=%u target=%u hidden=%u init_dma_us=%llu error=%d\n",
   (unsigned)s->token.high,(unsigned)s->token.low,(unsigned)s->allocationBytes,
   (unsigned)s->gpuAllocationBytes[0],(unsigned)s->gpuAllocationBytes[1],(unsigned)(s->gpuHidden[0]|(s->gpuHidden[1]<<1)),
   (unsigned long long)(initialized-start),error);
  if(error) { result=error==-ETIMEDOUT&&!s->gfxPending?kIOReturnBusy:error==-ECANCELED?kIOReturnNotReady:kIOReturnIOError; goto fail; }
 }
 {
  failureStage="uvd-create";
  int error=rdn_uvd_wake(&fUvd);
  if(error) { videoPoison(error); result=kIOReturnIOError; goto fail; }
  s->awake=true;
  error=videoCommand(RDN_UVD_H264_MSG_CREATE,0,0);
  if(error) { result=error==-EBUSY?kIOReturnBusy:error==-ECANCELED?kIOReturnNotReady:kIOReturnIOError; goto fail; }
 }
 s->created=true;
 IOLog("RadeonNI UVD memory session=%08x:%08x visible=0x%llx/%u dpb=0x%llx/%u target=0x%llx/%u hidden=%u\n",
  (unsigned)s->token.high,(unsigned)s->token.low,
  (unsigned long long)(fAccel.vram_base+s->allocatedAddress),(unsigned)s->allocationBytes,
  (unsigned long long)(fAccel.vram_base+s->gpuAllocation[0]),(unsigned)s->gpuAllocationBytes[0],
  (unsigned long long)(fAccel.vram_base+s->gpuAllocation[1]),(unsigned)s->gpuAllocationBytes[1],
  (unsigned)(s->gpuHidden[0]|(s->gpuHidden[1]<<1)));
 out->version=RDN_VIDEO_API_VERSION; out->bytes=sizeof(*out); out->session=s->token;
 out->coded_width=s->config.coded_width; out->coded_height=s->config.coded_height;
 out->format=RDN_VIDEO_FORMAT_NV12; out->y_stride=out->uv_stride=s->config.coded_width;
 out->uv_offset=s->config.coded_width*s->config.coded_height; out->output_bytes=layout.packed_nv12_bytes;
 out->upload_bytes=RDN_VIDEO_MAX_UPLOAD_BYTES; out->output_slots=2;
 out->upload_memory_type=RDN_VIDEO_MEMORY_UPLOAD; out->output0_memory_type=RDN_VIDEO_MEMORY_OUTPUT0; out->output1_memory_type=RDN_VIDEO_MEMORY_OUTPUT1;
 result=kIOReturnSuccess; goto done;
fail:
 IOLockLock(fLock);
 visibleFree=rdn_mem_available(&fMem); visibleLargest=rdn_mem_largest(&fMem);
 if(fHiddenSize) { hiddenFree=rdn_mem_available(&fHidden); hiddenLargest=rdn_mem_largest(&fHidden); }
 cpuBytes=fVideoCpuBytes;
 IOLockUnlock(fLock);
 IOLog("RadeonNI UVD create failed stage=%s result=0x%x visible=%u dpb=%u target=%u hidden=%u visible_free=%llu visible_largest=%llu hidden_free=%llu hidden_largest=%llu cpu=%u\n",
  failureStage,(unsigned)result,s?(unsigned)s->allocationBytes:0,
  s?(unsigned)s->gpuAllocationBytes[0]:0,s?(unsigned)s->gpuAllocationBytes[1]:0,
  s?(unsigned)(s->gpuHidden[0]|(s->gpuHidden[1]<<1)):0,
  (unsigned long long)visibleFree,(unsigned long long)visibleLargest,
  (unsigned long long)hiddenFree,(unsigned long long)hiddenLargest,(unsigned)cpuBytes);
 videoCleanup(false);
done:
 IOLockUnlock(fUvdLock); return result;
}
IOReturn RadeonNIAccel::videoDecode(RadeonNIVideoUserClient *owner,const rdn_video_decode *in,rdn_video_output *out)
{
 if(in->version!=RDN_VIDEO_API_VERSION || in->bytes!=sizeof(*in) || !videoZero(in->reserved,2) ||
    !in->bitstream_bytes || in->bitstream_bytes>RDN_VIDEO_MAX_UPLOAD_BYTES) return kIOReturnBadArgument;
 IOLockLock(fUvdLock);
 IOReturn result=kIOReturnNotReady;
 RadeonNIVideoSession *s=fVideoSession; UInt32 slot=0; uint32_t padded=0;
 bool timed=false; UInt64 snapshotStart=0,snapshotEnd=0;
 if(fRetiring || !s || s->owner!=owner || !s->created || s->quarantined || fUvd.failed) goto done;
 if(s->decodeFailed) { result=kIOReturnIOError; goto done; }
 if(!videoToken(in->session,s->token) || in->request_sequence!=s->nextRequest ||
    s->nextRequest==UINT32_MAX || s->nextOutput==UINT32_MAX) { result=kIOReturnBadArgument; goto done; }
 if(s->held[0]) slot=1;
 if(s->held[slot]) { result=kIOReturnBusy; goto done; }
 if(rdn_uvd_h264_bitstream_size(in->bitstream_bytes,&padded) || padded>RDN_VIDEO_MAX_UPLOAD_BYTES ||
    rdn_uvd_h264_pack_decode(&s->config,&in->picture,&s->layout,s->handle,s->nextRequest,
     in->bitstream_bytes,s->message,sizeof(s->message))) { result=kIOReturnBadArgument; goto done; }
 timed=s->validation && s->nextRequest<=8; snapshotStart=videoTimeUs(timed);
 memcpy(s->snapshot,s->upload->getBytesNoCopy(),in->bitstream_bytes);
 bzero(s->snapshot+in->bitstream_bytes,padded-in->bitstream_bytes);
 snapshotEnd=videoTimeUs(timed);
 {
  volatile UInt8 *base=(volatile UInt8 *)fAccel.aperture+s->allocation;
  videoWrite(base+s->offsets[kVideoBitstream],s->snapshot,padded);
  videoFill(base+s->offsets[kVideoFeedback],s->sizes[kVideoFeedback],0);
  base[s->offsets[kVideoFeedback]+1]=8;
  UInt64 uploaded=videoTimeUs(timed);
  int error=videoCommand(RDN_UVD_H264_MSG_DECODE,&in->picture,in->bitstream_bytes);
  if(error) { result=error==-EBUSY?kIOReturnBusy:error==-ECANCELED?kIOReturnNotReady:kIOReturnIOError; goto done; }
  UInt64 decoded=videoTimeUs(timed);
  volatile const UInt32 *feedback=(volatile const UInt32 *)(base+s->offsets[kVideoFeedback]);
  OSSynchronizeIO();
  UInt32 feedbackSequence=rdn_swap_le32(feedback[1]),feedbackStatus=rdn_swap_le32(feedback[2]);
  if(s->validation && s->nextRequest<=8) {
   UInt32 words[8];
   OSSynchronizeIO();
   for(unsigned i=0;i<8;++i) words[i]=rdn_swap_le32(feedback[i]);
   IOLog("RadeonNI UVD feedback session=%08x:%08x request=%u raw=%08x %08x %08x %08x %08x %08x %08x %08x\n",
    (unsigned)s->token.high,(unsigned)s->token.low,(unsigned)s->nextRequest,
    (unsigned)words[0],(unsigned)words[1],(unsigned)words[2],(unsigned)words[3],
    (unsigned)words[4],(unsigned)words[5],(unsigned)words[6],(unsigned)words[7]);
  }
  // Completed malformed pictures report a nonzero status; do not publish partial pixels.
  if(feedbackSequence!=s->nextRequest || feedbackStatus) {
   s->decodeFailed=true; result=kIOReturnIOError; goto done;
  }
  UInt64 diagnosed=videoTimeUs(timed);
  error=videoReadback();
  if(error) { result=kIOReturnIOError; goto done; }
  UInt64 copied=videoTimeUs(timed);
  if(!videoGuards(s)) { videoPoison(-EIO); result=kIOReturnIOError; goto done; }
  UInt64 checked=videoTimeUs(timed);
  UInt8 *dst=(UInt8 *)s->output[slot]->getBytesNoCopy();
  const UInt8 *pixels=(const UInt8 *)s->readback->getBytesNoCopy();
  UInt32 width=s->config.coded_width,height=s->config.coded_height;
  for(UInt32 row=0;row<height;++row) {
   UInt32 offset=(row&1)?s->layout.luma_bottom_offset:s->layout.luma_top_offset;
   offset+=(row/2)*s->layout.target_pitch;
   memcpy(dst+row*width,pixels+offset,width);
  }
  for(UInt32 row=0;row<height/2;++row) {
   UInt32 offset=(row&1)?s->layout.chroma_bottom_offset:s->layout.chroma_top_offset;
   offset+=(row/2)*s->layout.target_pitch;
   memcpy(dst+width*height+row*width,pixels+offset,width);
  }
  UInt64 woven=videoTimeUs(timed);
  if(timed) IOLog("RadeonNI UVD decode timing session=%08x:%08x request=%u bitstream=%u snapshot_us=%llu upload_us=%llu uvd_us=%llu diagnostic_us=%llu readback_us=%llu guards_us=%llu weave_us=%llu\n",
   (unsigned)s->token.high,(unsigned)s->token.low,(unsigned)s->nextRequest,(unsigned)in->bitstream_bytes,
   (unsigned long long)(snapshotEnd-snapshotStart),(unsigned long long)(uploaded-snapshotEnd),
   (unsigned long long)(decoded-uploaded),(unsigned long long)(diagnosed-decoded),
   (unsigned long long)(copied-diagnosed),(unsigned long long)(checked-copied),(unsigned long long)(woven-checked));
  s->held[slot]=s->nextOutput++;
  out->version=RDN_VIDEO_API_VERSION; out->bytes=sizeof(*out); out->session=s->token;
  out->request_sequence=s->nextRequest++; out->output_sequence=s->held[slot]; out->slot=slot;
  out->coded_width=width; out->coded_height=height; out->format=RDN_VIDEO_FORMAT_NV12;
  out->y_stride=out->uv_stride=width; out->uv_offset=width*height; out->output_bytes=s->layout.packed_nv12_bytes;
  result=kIOReturnSuccess;
 }
done:
 IOLockUnlock(fUvdLock); return result;
}
IOReturn RadeonNIAccel::videoRelease(RadeonNIVideoUserClient *owner,const rdn_video_release *in)
{
 if(in->version!=RDN_VIDEO_API_VERSION || in->bytes!=sizeof(*in) || !videoZero(in->reserved,2) || in->slot>=2 || !in->output_sequence) return kIOReturnBadArgument;
 IOLockLock(fUvdLock);
 RadeonNIVideoSession *s=fVideoSession; IOReturn result=kIOReturnBadArgument;
 if(!fRetiring && s && s->owner==owner && videoToken(s->token,in->session) && s->held[in->slot]==in->output_sequence) {
  s->held[in->slot]=0; result=kIOReturnSuccess;
 }
 IOLockUnlock(fUvdLock); return result;
}
IOReturn RadeonNIAccel::videoDestroy(RadeonNIVideoUserClient *owner,const rdn_video_destroy *in)
{
 if(in->version!=RDN_VIDEO_API_VERSION || in->bytes!=sizeof(*in) || !videoZero(in->reserved,4)) return kIOReturnBadArgument;
 IOLockLock(fUvdLock);
 IOReturn result=kIOReturnBadArgument;
 if(fVideoSession && fVideoSession->owner==owner && videoToken(fVideoSession->token,in->session)) {
  videoCleanup(true); result=fVideoSession?kIOReturnIOError:kIOReturnSuccess;
 }
 IOLockUnlock(fUvdLock); return result;
}
void RadeonNIAccel::videoClose(RadeonNIVideoUserClient *owner)
{
 IOLockLock(fUvdLock);
 if(fVideoSession && (!owner || fVideoSession->owner==owner)) videoCleanup(true);
 IOLockUnlock(fUvdLock);
}
IOReturn RadeonNIAccel::videoMemory(RadeonNIVideoUserClient *owner,UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
 IOLockLock(fUvdLock);
 IOReturn result=kIOReturnNotReady;
 RadeonNIVideoSession *s=fVideoSession; RadeonNIVideoMemory *descriptor=0;
 if(!fRetiring && s && s->owner==owner && s->created && !s->quarantined) {
  if(type==RDN_VIDEO_MEMORY_UPLOAD) descriptor=s->upload;
  else if(type==RDN_VIDEO_MEMORY_OUTPUT0 || type==RDN_VIDEO_MEMORY_OUTPUT1) descriptor=s->output[type-RDN_VIDEO_MEMORY_OUTPUT0];
  if(descriptor) {
   *options=(*options&~kIOMapCacheMask)|kIOMapDefaultCache;
   if(type!=RDN_VIDEO_MEMORY_UPLOAD) *options|=kIOMapReadOnly;
   descriptor->retain(); *memory=descriptor; result=kIOReturnSuccess;
  } else result=kIOReturnBadArgument;
 }
 IOLockUnlock(fUvdLock); return result;
}
