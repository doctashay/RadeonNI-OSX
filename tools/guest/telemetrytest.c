/* Exercise read-only telemetry on every physical RadeonNI framebuffer. */
#include <stdio.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include "rdn_user.h"
int main(void)
{
 io_iterator_t it=0;io_service_t fb;unsigned count=0,failed=0;
 if(IOServiceGetMatchingServices(0,IOServiceMatching("RadeonNI"),&it))return 1;
 while((fb=IOIteratorNext(it))) {
  io_registry_entry_t pci=0;
  if(IORegistryEntryGetParentEntry(fb,kIOServicePlane,&pci)) {IOObjectRelease(fb);continue;}
  if(!IOObjectConformsTo(pci,"IOPCIDevice")) {IOObjectRelease(pci);IOObjectRelease(fb);continue;}
  io_string_t path;IORegistryEntryGetPath(pci,kIOServicePlane,path);IOObjectRelease(pci);
  io_connect_t c=0;kern_return_t e=IOServiceOpen(fb,mach_task_self(),RDN_UC_TELEMETRY,&c);
  IOObjectRelease(fb);count++;
  if(e) {printf("FAIL open %s %08x\n",path,e);failed++;continue;}
  uint32_t raw=0,core=0,mc=0,sc=0,memclk=0;
  e=IOConnectMethodScalarIScalarO(c,RDN_TELEMETRY_SAMPLE,0,3,&raw,&core,&mc);
  kern_return_t ce=IOConnectMethodScalarIScalarO(c,RDN_TELEMETRY_CLOCKS,0,2,&sc,&memclk);
  struct rdn_user_board_sensors board; IOByteCount boardSize=sizeof(board);
  kern_return_t be=IOConnectMethodScalarIStructureO(c,RDN_TELEMETRY_BOARD,0,&boardSize,&board);
  kern_return_t invalid=IOConnectMethodScalarIScalarO(c,3,0,0);
  if(!be)printf("board fan_status=%u rpm=%u period=%u clock=%u pulses=%u edges=%u rail_status=%u uV=%u controller=%x\n",board.fan_status,board.fan_rpm,board.tach_period,board.tach_clock_hz,board.fan_pulses,board.tach_edges,board.rail_status,board.rail_microvolts,board.voltage_control_id);
  if(be || boardSize!=sizeof(board) || board.version!=1 || board.size!=sizeof(board))failed++;
  kern_return_t wrong=IOConnectMethodScalarIScalarO(c,RDN_TELEMETRY_SAMPLE,1,3,0x740,&raw,&core,&mc);
  printf("%s sample=%08x clocks=%08x raw=%08x core=%08x mc=%08x MHz=%.2f/%.2f invalid=%08x wrong_arity=%08x\n",path,e,ce,raw,core,mc,sc/100.,memclk/100.,invalid,wrong);
  if(e||ce||!sc||!memclk||!invalid||!wrong)failed++;
  IOServiceClose(c);
 }
 IOObjectRelease(it);printf("%s: %u physical GPU telemetry connections\n",failed||count<2?"FAIL":"PASS",count);
 return failed||count<2;
}
