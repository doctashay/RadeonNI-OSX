#ifndef RADEONNIVIDEO_H
#define RADEONNIVIDEO_H
#include "RadeonNIAccel.h"
class RadeonNIVideoUserClient : public IOUserClient {
 OSDeclareDefaultStructors(RadeonNIVideoUserClient)
public:
 virtual bool initWithTask(task_t task, void *securityID, UInt32 type);
 virtual bool start(IOService *provider);
 virtual void free(void);
 virtual IOReturn clientClose(void);
 virtual IOReturn clientDied(void);
 virtual void stop(IOService *provider);
 virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index);
 virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);
 IOReturn methodQuery(struct rdn_video_query *out, IOByteCount *size);
 IOReturn methodCreate(const struct rdn_video_create *in, struct rdn_video_session *out, IOByteCount inSize, IOByteCount *outSize);
 IOReturn methodDecode(const struct rdn_video_decode *in, struct rdn_video_output *out, IOByteCount inSize, IOByteCount *outSize);
 IOReturn methodDecodeGpu(const struct rdn_video_decode *in, struct rdn_video_gpu_output *out, IOByteCount inSize, IOByteCount *outSize);
 IOReturn methodAcquireGpu(const struct rdn_video_gpu_acquire *in, struct rdn_video_gpu_source *out, IOByteCount inSize, IOByteCount *outSize);
 IOReturn methodRetireGpu(const struct rdn_video_gpu_retire *in, IOByteCount size);
 IOReturn methodRelease(const struct rdn_video_release *in, IOByteCount size);
 IOReturn methodDestroy(const struct rdn_video_destroy *in, IOByteCount size);
 bool closed(void) const { return fClosed; }
private:
 RadeonNIAccel *fAccel;
 IOLock *fClientLock;
 bool fAdministrator, fClosed, fCreated;
};
#endif
