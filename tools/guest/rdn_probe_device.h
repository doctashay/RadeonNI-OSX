/* Select a physical GPU without relying on registry enumeration order. */
#ifndef RDN_PROBE_DEVICE_H
#define RDN_PROBE_DEVICE_H
#include <stdlib.h>
static io_service_t rdn_probe_device(const char *device)
{
 io_iterator_t it = 0;
 io_service_t service, selected = 0;
 unsigned long wanted = 0;
 if (device) {
  char *end;
  wanted = strtoul(device, &end, 16);
  if (!*device || *end || !wanted || wanted > 0xffff) return 0;
 }
 if (IOServiceGetMatchingServices(kIOMasterPortDefault,
     IOServiceMatching(RDN_UC_SERVICE_CLASS), &it)) return 0;
 while ((service = IOIteratorNext(it))) {
  io_connect_t c = 0;
  struct rdn_user_info info;
  IOByteCount bytes = sizeof(info);
  memset(&info, 0, sizeof(info));
  kern_return_t e = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE_V2, &c);
  if (!e) {
#ifdef __LP64__
   size_t length = sizeof(info);
   e = IOConnectCallStructMethod(c, RDN_UC_GET_INFO, NULL, 0, &info, &length);
   bytes = length;
#else
   e = IOConnectMethodScalarIStructureO(c, RDN_UC_GET_INFO, 0, &bytes, &info);
#endif
   IOServiceClose(c);
  }
  if (!e && bytes == sizeof(info) && info.version == RDN_USER_VERSION &&
      (!wanted || info.pci_device_id == wanted)) {
   if (selected) {
    IOObjectRelease(service); IOObjectRelease(selected); selected = 0;
    fprintf(stderr, "GPU selection is ambiguous; specify a unique PCI device ID\n");
    break;
   }
   selected = service;
  } else IOObjectRelease(service);
 }
 IOObjectRelease(it);
 return selected;
}
#endif
