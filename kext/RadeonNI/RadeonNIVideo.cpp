#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include "RadeonNIVideo.h"
#define super IOUserClient
OSDefineMetaClassAndStructors(RadeonNIVideoUserClient, IOUserClient)

bool RadeonNIVideoUserClient::initWithTask(task_t task, void *securityID, UInt32 type)
{
 if (!super::initWithTask(task, securityID, type)) return false;
 fAccel = 0; fClosed = false; fCreated = false;
 fAdministrator = clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
 fClientLock = IOLockAlloc();
 return fClientLock != 0;
}
bool RadeonNIVideoUserClient::start(IOService *provider)
{
 RadeonNIAccel *accel = OSDynamicCast(RadeonNIAccel, provider);
 if (!accel || !super::start(provider)) return false;
 fAccel = accel; fAccel->retain();
 return true;
}
void RadeonNIVideoUserClient::free(void)
{
 if (fClientLock) { IOLockFree(fClientLock); fClientLock = 0; }
 super::free();
}
IOReturn RadeonNIVideoUserClient::clientClose(void)
{
 IOLockLock(fClientLock);
 if (!fClosed) { fClosed = true; if (fAccel) fAccel->videoClose(this); }
 IOLockUnlock(fClientLock);
 terminate();
 return kIOReturnSuccess;
}
IOReturn RadeonNIVideoUserClient::clientDied(void) { return clientClose(); }
void RadeonNIVideoUserClient::stop(IOService *provider)
{
 IOLockLock(fClientLock);
 fClosed = true;
 RadeonNIAccel *accel = fAccel;
 if (accel) accel->videoClose(this);
 fAccel = 0;
 IOLockUnlock(fClientLock);
 if (accel) accel->release();
 super::stop(provider);
}
IOExternalMethod *RadeonNIVideoUserClient::getTargetAndMethodForIndex(IOService **target, UInt32 index)
{
 static const IOExternalMethod methods[RDN_VIDEO_METHOD_COUNT] = {
  {0,(IOMethod)&RadeonNIVideoUserClient::methodQuery,kIOUCScalarIStructO,0,sizeof(rdn_video_query)},
  {0,(IOMethod)&RadeonNIVideoUserClient::methodCreate,kIOUCStructIStructO,sizeof(rdn_video_create),sizeof(rdn_video_session)},
  {0,(IOMethod)&RadeonNIVideoUserClient::methodDecode,kIOUCStructIStructO,sizeof(rdn_video_decode),sizeof(rdn_video_output)},
  {0,(IOMethod)&RadeonNIVideoUserClient::methodRelease,kIOUCScalarIStructI,0,sizeof(rdn_video_release)},
  {0,(IOMethod)&RadeonNIVideoUserClient::methodDestroy,kIOUCScalarIStructI,0,sizeof(rdn_video_destroy)}
 };
 if (index >= RDN_VIDEO_METHOD_COUNT) return 0;
 *target = this;
 return const_cast<IOExternalMethod *>(&methods[index]);
}
IOReturn RadeonNIVideoUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
 if (!options || !memory) return kIOReturnBadArgument;
 *memory = 0;
 IOLockLock(fClientLock);
 IOReturn result = !fClosed && fAccel ? fAccel->videoMemory(this,type,options,memory) : kIOReturnNotOpen;
 IOLockUnlock(fClientLock);
 return result;
}
IOReturn RadeonNIVideoUserClient::methodQuery(rdn_video_query *out, IOByteCount *size)
{
 if (!out || !size || *size != sizeof(*out)) return kIOReturnBadArgument;
 bzero(out,sizeof(*out));
 IOLockLock(fClientLock);
 IOReturn result = kIOReturnNotOpen;
 if (!fClosed && fAccel) { fAccel->videoQuery(fAdministrator,out); result=kIOReturnSuccess; }
 IOLockUnlock(fClientLock);
 return result;
}
IOReturn RadeonNIVideoUserClient::methodCreate(const rdn_video_create *in, rdn_video_session *out, IOByteCount inSize, IOByteCount *outSize)
{
 if (!in || !out || !outSize || inSize!=sizeof(*in) || *outSize!=sizeof(*out)) return kIOReturnBadArgument;
 bzero(out,sizeof(*out));
 IOLockLock(fClientLock);
 IOReturn result=kIOReturnNotOpen;
 if (!fClosed && fAccel) {
  result=fCreated ? kIOReturnExclusiveAccess : fAccel->videoCreate(this,fAdministrator,in,out);
  if (result==kIOReturnSuccess) fCreated=true;
 }
 IOLockUnlock(fClientLock);
 return result;
}
IOReturn RadeonNIVideoUserClient::methodDecode(const rdn_video_decode *in, rdn_video_output *out, IOByteCount inSize, IOByteCount *outSize)
{
 if (!in || !out || !outSize || inSize!=sizeof(*in) || *outSize!=sizeof(*out)) return kIOReturnBadArgument;
 bzero(out,sizeof(*out));
 IOLockLock(fClientLock);
 IOReturn result=!fClosed && fAccel ? fAccel->videoDecode(this,in,out) : kIOReturnNotOpen;
 IOLockUnlock(fClientLock);
 return result;
}
IOReturn RadeonNIVideoUserClient::methodRelease(const rdn_video_release *in, IOByteCount size)
{
 if (!in || size!=sizeof(*in)) return kIOReturnBadArgument;
 IOLockLock(fClientLock);
 IOReturn result=!fClosed && fAccel ? fAccel->videoRelease(this,in) : kIOReturnNotOpen;
 IOLockUnlock(fClientLock);
 return result;
}
IOReturn RadeonNIVideoUserClient::methodDestroy(const rdn_video_destroy *in, IOByteCount size)
{
 if (!in || size!=sizeof(*in)) return kIOReturnBadArgument;
 IOLockLock(fClientLock);
 IOReturn result=!fClosed && fAccel ? fAccel->videoDestroy(this,in) : kIOReturnNotOpen;
 IOLockUnlock(fClientLock);
 return result;
}
