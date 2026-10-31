#ifndef RADEONNI_TELEMETRY_H
#define RADEONNI_TELEMETRY_H
#include <IOKit/IOUserClient.h>
class RadeonNI;
struct rdn_user_board_sensors;
class RadeonNITelemetry : public IOUserClient {
 OSDeclareDefaultStructors(RadeonNITelemetry)
 RadeonNI *fFramebuffer;
public:
 virtual bool start(IOService *provider);
 virtual IOReturn clientClose();
 virtual IOReturn clientDied();
 virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target,UInt32 index);
 IOReturn sample(UInt32 *temperature,UInt32 *core,UInt32 *memory);
 IOReturn clocks(UInt32 *core,UInt32 *memory);
 IOReturn board(struct rdn_user_board_sensors *out, IOByteCount *size);
};
#endif
