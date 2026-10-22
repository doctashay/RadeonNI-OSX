#ifndef RADEONNIVIDEOMEMORY_H
#define RADEONNIVIDEOMEMORY_H
#include "RadeonNIAccel.h"
class RadeonNIVideoMemory : public IOGeneralMemoryDescriptor {
 OSDeclareDefaultStructors(RadeonNIVideoMemory)
public:
 static RadeonNIVideoMemory *withBytes(RadeonNIAccel *owner, UInt32 bytes, bool readOnly);
 void *getBytesNoCopy(void) { return fBuffer; }
 virtual IOReturn doMap(vm_map_t map, IOVirtualAddress *address, IOOptionBits options,
                       IOByteCount offset=0, IOByteCount length=0);
protected:
 virtual void free(void);
private:
 RadeonNIAccel *fOwner;
 void *fBuffer;
 UInt32 fBytes;
 bool fReadOnly;
};
#endif
