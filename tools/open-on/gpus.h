#ifndef RDN_OPEN_ON_GPUS_H
#define RDN_OPEN_ON_GPUS_H
#include <Carbon/Carbon.h>
#include <IOKit/IOKitLib.h>
#include <string.h>
struct open_gpu { int index, ready; CFStringRef name; };
static CFStringRef gpu_text(CFTypeRef value)
{
 if(!value)return NULL;
 if(CFGetTypeID(value)==CFStringGetTypeID())return CFRetain(value);
 if(CFGetTypeID(value)!=CFDataGetTypeID())return NULL;
 const UInt8 *p=CFDataGetBytePtr(value);CFIndex n=CFDataGetLength(value),i=0;
 while(i<n && p[i])i++;
 CFStringRef s=CFStringCreateWithBytes(NULL,p,i,kCFStringEncodingUTF8,false);
 return s?s:CFStringCreateWithBytes(NULL,p,i,kCFStringEncodingMacRoman,false);
}
static unsigned open_gpus(struct open_gpu *out,unsigned capacity)
{
 io_iterator_t it=0;io_service_t service;unsigned count=0;
 if(IOServiceGetMatchingServices(kIOMasterPortDefault,IOServiceMatching("RadeonNIAccel"),&it))return 0;
 while((service=IOIteratorNext(it))) {
  CFTypeRef value=IORegistryEntryCreateCFProperty(service,CFSTR("GLDDeviceIndex"),NULL,0);
  int index=-1;
  if(value && CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(value,kCFNumberIntType,&index);
  if(value)CFRelease(value);
  if(index>=0 && index<4 && count<capacity) {
   CFOptionFlags flags=kIORegistryIterateRecursively|kIORegistryIterateParents;
   io_name_t plane=kIOServicePlane;
   value=IORegistryEntrySearchCFProperty(service,plane,CFSTR("model"),NULL,flags);
   CFStringRef model=gpu_text(value);if(value)CFRelease(value);
   value=IORegistryEntrySearchCFProperty(service,plane,CFSTR("AAPL,slot-name"),NULL,flags);
   CFStringRef slot=gpu_text(value);if(value)CFRelease(value);
   value=IORegistryEntryCreateCFProperty(service,CFSTR("HardwarePowerFault"),NULL,0);
   out[count].index=index;out[count].ready=value!=kCFBooleanTrue;
   if(value)CFRelease(value);
   out[count].name=CFStringCreateWithFormat(NULL,NULL,CFSTR("%@%@%@%@"),model?model:CFSTR("Radeon GPU"),slot?CFSTR(" ("):CFSTR(""),slot?slot:CFSTR(""),slot?CFSTR(")"):CFSTR(""));
   if(model)CFRelease(model);if(slot)CFRelease(slot);count++;
  }
  IOObjectRelease(service);
 }
 IOObjectRelease(it);return count;
}
static void free_gpus(struct open_gpu *g,unsigned count)
{
 for(unsigned i=0;i<count;i++)if(g[i].name)CFRelease(g[i].name);
}
#endif
