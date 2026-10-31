#include "RadeonNIAccel.h"
#include "RadeonNI.h"
#include <IOKit/IOLib.h>

/* Stable GPU aliases for private Mesa storage. */
IOReturn RadeonNIAccel::managedInfo(UInt32 *version)
{
 *version = fGart.ready ? 1 : 0;
 return kIOReturnSuccess;
}

IOReturn RadeonNIAccel::managedAlias(UInt32 physical, UInt32 size, UInt32 alignment,
                                     void *owner, UInt32 *alias)
{
 if (!size || (size & 4095) || (physical & 4095) || !alignment ||
     (alignment & (alignment - 1))) return kIOReturnBadArgument;
 IOLockLock(fLock);
 IOReturn result = kIOReturnNoResources;
 UInt32 slot = 0;
 uint64_t at = 0;
 struct rdn_mem *heap = physical >= fHiddenOffset && fHiddenSize ? &fHidden : &fMem;
 if (!fGart.ready || fSurfaceGpuStalled || size > rdn_mem_allocation_size(heap, physical)) {
  IOLockUnlock(fLock); return kIOReturnBadArgument;
 }
 for (; slot < kMaxGartBindings && (fGartBound[slot].memory || fGartBound[slot].vram); ++slot) {}
 /* Leave the first GiB available for ordinary uploads and scanout transfers. */
 if (slot < kMaxGartBindings && !rdn_mem_alloc_range(&fGartSpace, size,
     alignment < 4096 ? 4096 : alignment, 1ull << 30, 1ull << 32, &at)) {
  fGartBound[slot].offset = (UInt32)at; fGartBound[slot].size = size;
  fGartBound[slot].vram = physical; fGartBound[slot].owner = owner;
  for (UInt32 done = 0; done < size; done += 4096)
   rdn_gart_set_vram_page(&fGart, (UInt32)(at + done) / 4096,
                         rdn_vram_addr(&fAccel, physical + done));
  if (!rdn_gart_flush_checked(&fGart)) { *alias = (UInt32)at; result = kIOReturnSuccess; }
  else { fSurfaceGpuStalled = true; result = kIOReturnTimeout; }
 }
 IOLockUnlock(fLock);
 return result;
}

IOReturn RadeonNIAccel::managedEvict(UInt32 alias, task_t task, UInt64 address, void *owner)
{
 if (address & 4095) return kIOReturnBadArgument;
 IOLockLock(fLock);
 UInt32 slot;
 for (slot = 0; slot < kMaxGartBindings; ++slot)
  if (fGartBound[slot].vram && !fGartBound[slot].memory &&
      fGartBound[slot].owner == owner && fGartBound[slot].offset == alias) break;
 if (slot == kMaxGartBindings || fSurfaceGpuStalled) {
  IOLockUnlock(fLock); return kIOReturnNotReady;
 }
 UInt32 size = fGartBound[slot].size, physical = fGartBound[slot].vram;
 IOLockUnlock(fLock);
#ifdef RDN_LEOPARD
 IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, size, kIODirectionOutIn, task);
#else
 if (address > 0xffffffffull) return kIOReturnUnsupported;
 IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddress((vm_address_t)address, size, kIODirectionOutIn, task);
#endif
 if (!memory) return kIOReturnNoMemory;
 if (memory->prepare() != kIOReturnSuccess) { memory->release(); return kIOReturnVMError; }
 /* Validate every page before changing a live mapping. */
 for (UInt32 at = 0; at < size; at += 4096) {
  IOByteCount length = 0;
  if (!memory->getPhysicalSegment(at, &length) || length < 4096) {
   memory->complete(); memory->release(); return kIOReturnVMError;
  }
 }
 IOLockLock(fLock);
 if (fSurfaceGpuStalled || fGartBound[slot].owner != owner ||
     fGartBound[slot].vram != physical || fGartBound[slot].memory ||
     rdn_fence_wait(&fAccel, fAccel.fence_emitted, 2000)) {
  IOLockUnlock(fLock); memory->complete(); memory->release(); return kIOReturnBusy;
 }
 fGartBound[slot].memory = memory;
 for (UInt32 at = 0; at < size; at += 4096) {
  IOByteCount length = 0;
  rdn_gart_set_page(&fGart, (alias + at) / 4096, memory->getPhysicalSegment(at, &length));
 }
 int error = rdn_gart_flush_checked(&fGart);
 for (UInt32 at = 0; !error && at < size;) {
  UInt32 count = size - at > (1u << 20) ? 1u << 20 : size - at;
  uint32_t fence = 0;
  error = rdn_cp_dma(&fAccel, rdn_vram_addr(&fAccel, physical + at),
                    gartAddress(alias) + at, count, &fence);
  if (!error) error = rdn_fence_wait(&fAccel, fence, 2000);
  at += count;
 }
 if (error) fSurfaceGpuStalled = true; /* Both mappings and allocations stay pinned. */
 else fGartBound[slot].vram = 0;
 IOLockUnlock(fLock);
 return error ? kIOReturnTimeout : kIOReturnSuccess;
}

IOReturn RadeonNIAccel::managedPromote(UInt32 alias, UInt32 physical, void *owner)
{
 IOLockLock(fLock);
 UInt32 slot;
 for (slot = 0; slot < kMaxGartBindings; ++slot)
  if (fGartBound[slot].memory && !fGartBound[slot].vram &&
      fGartBound[slot].owner == owner && fGartBound[slot].offset == alias) break;
 if (slot == kMaxGartBindings || fSurfaceGpuStalled) { IOLockUnlock(fLock); return kIOReturnNotReady; }
 UInt32 size = fGartBound[slot].size;
 struct rdn_mem *heap = physical >= fHiddenOffset && fHiddenSize ? &fHidden : &fMem;
 if ((physical & 4095) || size > rdn_mem_allocation_size(heap, physical)) {
  IOLockUnlock(fLock); return kIOReturnBadArgument;
 }
 int error = rdn_fence_wait(&fAccel, fAccel.fence_emitted, 2000);
 for (UInt32 at = 0; !error && at < size;) {
  UInt32 count = size - at > (1u << 20) ? 1u << 20 : size - at;
  uint32_t fence = 0;
  error = rdn_cp_dma(&fAccel, gartAddress(alias) + at,
                    rdn_vram_addr(&fAccel, physical + at), count, &fence);
  if (!error) error = rdn_fence_wait(&fAccel, fence, 2000);
  at += count;
 }
 if (!error) {
  for (UInt32 at = 0; at < size; at += 4096)
   rdn_gart_set_vram_page(&fGart, (alias + at) / 4096, rdn_vram_addr(&fAccel, physical + at));
  error = rdn_gart_flush_checked(&fGart);
 }
 IOMemoryDescriptor *memory = fGartBound[slot].memory;
 if (error) fSurfaceGpuStalled = true;
 else { fGartBound[slot].memory = 0; fGartBound[slot].vram = physical; }
 IOLockUnlock(fLock);
 if (!error) { memory->complete(); memory->release(); }
 return error ? kIOReturnTimeout : kIOReturnSuccess;
}

IOReturn RadeonNIUserClient::methodManagedInfo(UInt32 *version)
{ return fAccel->managedInfo(version); }

IOReturn RadeonNIUserClient::methodManagedAlias(UInt32 physical, UInt32 size,
                                               UInt32 alignment, UInt32 *alias)
{
 IOLockLock(fAllocationLock);
 IOReturn result = kIOReturnBadArgument;
 for (UInt32 i = 0; i < fCount; ++i)
  if (fOffsets[i] == physical) { result = fAccel->managedAlias(physical, size, alignment, this, alias); break; }
 IOLockUnlock(fAllocationLock); return result;
}

IOReturn RadeonNIUserClient::methodManagedEvict(UInt32 alias, UInt32 lo, UInt32 hi)
{
 IOLockLock(fAllocationLock);
 IOReturn result = fAccel->managedEvict(alias, fTask, ((UInt64)hi << 32) | lo, this);
 if (result == kIOReturnTimeout) fSurfaceGpuFailed = true;
 IOLockUnlock(fAllocationLock); return result;
}

IOReturn RadeonNIUserClient::methodManagedPromote(UInt32 alias, UInt32 physical)
{
 IOLockLock(fAllocationLock);
 IOReturn result = kIOReturnBadArgument;
 for (UInt32 i = 0; i < fCount; ++i)
  if (fOffsets[i] == physical) { result = fAccel->managedPromote(alias, physical, this); break; }
 if (result == kIOReturnTimeout) fSurfaceGpuFailed = true;
 IOLockUnlock(fAllocationLock); return result;
}
