#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include "RadeonNIVideoMemory.h"
#define super IOGeneralMemoryDescriptor
OSDefineMetaClassAndStructors(RadeonNIVideoMemory, IOGeneralMemoryDescriptor)

bool RadeonNIAccel::videoCharge(UInt32 bytes)
{
 UInt32 old;
 do { old=fVideoCpuBytes; if(old>20u*1024*1024 || bytes>20u*1024*1024-old) return false; }
 while(!OSCompareAndSwap(old,old+bytes,&fVideoCpuBytes));
 return true;
}
void RadeonNIAccel::videoUncharge(UInt32 bytes)
{
 OSAddAtomic(-(SInt32)bytes,(volatile SInt32 *)&fVideoCpuBytes);
}
RadeonNIVideoMemory *RadeonNIVideoMemory::withBytes(RadeonNIAccel *owner,UInt32 bytes,bool readOnly)
{
 if(!bytes || bytes>20u*1024*1024-4095) return 0;
 bytes=(bytes+4095)&~4095u;
 RadeonNIVideoMemory *memory=new RadeonNIVideoMemory;
 if(!memory) return 0;
 memory->fOwner=0; memory->fBuffer=0; memory->fBytes=0; memory->fReadOnly=readOnly;
 if(!owner->videoCharge(bytes)) { memory->release(); return 0; }
 owner->retain(); memory->fOwner=owner; memory->fBytes=bytes;
 memory->fBuffer=IOMallocAligned(bytes,4096);
 if(!memory->fBuffer) { memory->release(); return 0; }
 bzero(memory->fBuffer,bytes);
 if(!memory->initWithAddress(memory->fBuffer,bytes,kIODirectionInOut)) { memory->release(); return 0; }
 return memory;
}
IOReturn RadeonNIVideoMemory::doMap(vm_map_t map,IOVirtualAddress *address,IOOptionBits options,IOByteCount offset,IOByteCount length)
{
 /* Leopard grants write access when a mapping requests cache-mode changes. */
 if((options&kIOMapCacheMask)!=kIOMapDefaultCache || (fReadOnly && !(options&kIOMapReadOnly))) return kIOReturnNotPermitted;
 return super::doMap(map,address,options,offset,length);
}
void RadeonNIVideoMemory::free(void)
{
 RadeonNIAccel *owner=fOwner; void *buffer=fBuffer; UInt32 bytes=fBytes;
 /* Unwire and release VM entries before freeing the underlying pages. */
 super::free();
 if(buffer) IOFreeAligned(buffer,bytes);
 if(owner) { owner->videoUncharge(bytes); owner->release(); }
}
