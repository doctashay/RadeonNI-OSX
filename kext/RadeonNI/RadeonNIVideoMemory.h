#ifndef RADEONNIVIDEOMEMORY_H
#define RADEONNIVIDEOMEMORY_H
#include "RadeonNIAccel.h"
#ifdef RDN_LEOPARD
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
#else
/*
 * Not on Tiger: IOGeneralMemoryDescriptor is a private class, and 10.4.11's
 * does not match what Tiger's headers say, so the kernel refuses the subclass
 * when the kext links ("RadeonNIVideoMemory is not compatible with its
 * superclass"; the whole kext then fails to load). Video sessions are made
 * only on Leopard; here no session can be created, and this class is never
 * instantiated and has no metaclass of its own (the kernel sees nothing).
 */
class RadeonNIVideoMemory : public IOMemoryDescriptor {
public:
 static RadeonNIVideoMemory *withBytes(RadeonNIAccel *, UInt32, bool) { return 0; }
 void *getBytesNoCopy(void) { return 0; }
};
#endif
#endif
