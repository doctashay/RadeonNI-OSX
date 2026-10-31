/* Telemetry is independent of the optional 3D accelerator service. */
#include "RadeonNI.h"
#include "rdn_user.h"
#include "RadeonNITelemetry.h"
OSDefineMetaClassAndStructors(RadeonNITelemetry,IOUserClient)
bool RadeonNITelemetry::start(IOService *provider)
{
 fFramebuffer=OSDynamicCast(RadeonNI,provider);
 return fFramebuffer && IOUserClient::start(provider);
}
IOReturn RadeonNITelemetry::clientClose() { terminate();return kIOReturnSuccess; }
IOReturn RadeonNITelemetry::clientDied() { return clientClose(); }
IOExternalMethod *RadeonNITelemetry::getTargetAndMethodForIndex(IOService **target,UInt32 index)
{
 static const IOExternalMethod methods[]={
  {0,(IOMethod)&RadeonNITelemetry::sample,kIOUCScalarIScalarO,0,3},
  {0,(IOMethod)&RadeonNITelemetry::clocks,kIOUCScalarIScalarO,0,2},
  {0,(IOMethod)&RadeonNITelemetry::board,kIOUCScalarIStructO,0,sizeof(struct rdn_user_board_sensors)}
 };
 if(index>=3 || isInactive() || !fFramebuffer || fFramebuffer->isInactive())return 0;
 *target=this;return (IOExternalMethod *)&methods[index];
}
IOReturn RadeonNITelemetry::sample(UInt32 *temperature,UInt32 *core,UInt32 *memory)
{
 return fFramebuffer->readTelemetry(false,temperature,core,memory);
}
IOReturn RadeonNITelemetry::clocks(UInt32 *core,UInt32 *memory)
{
 UInt32 unused=0;return fFramebuffer->readTelemetry(true,&unused,core,memory);
}

IOReturn RadeonNITelemetry::board(struct rdn_user_board_sensors *out, IOByteCount *size)
{
 if (*size < sizeof(*out)) return kIOReturnBadArgument;
 IOReturn result = fFramebuffer->readBoardSensors(out);
 if (result == kIOReturnSuccess) *size = sizeof(*out);
 return result;
}
