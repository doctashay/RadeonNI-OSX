/* Per-device telemetry and guarded SMC performance control for Leopard. */
#import <Cocoa/Cocoa.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <string.h>
#include "rdn_user.h"

#define MAX_GPUS 4
static double memoryValue(struct rdn_user_bytes value)
{
 return (double)value.hi*4294967296.0+value.lo;
}

static NSString *sensorStatus(unsigned status)
{
 switch(status) {
 case RDN_SENSOR_VALID: return @"Valid";
 case RDN_SENSOR_NOT_IMPLEMENTED: return @"Driver sensor backend not implemented";
 case RDN_SENSOR_NOT_PRESENT: return @"No tachometer declared by BIOS";
 case RDN_SENSOR_INVALID: return @"Invalid counter or calibration";
 case RDN_SENSOR_NO_DESCRIPTION: return @"BIOS sensor description unavailable";
 default: return @"Unknown status";
 }
}

struct gpu_sensor {
 io_service_t framebuffer;
 io_connect_t connection,control;
 io_service_t accelerator;
 unsigned device;
 uint32_t vramBytes;
 uint64_t copyBytes,copyTime;
 NSString *name;
 uint64_t previous,total,core,memory,nonDisplay;
 uint32_t oldCore,oldMemory;
 unsigned samples,attempts;
 BOOL havePrevious,telemetry;
 uint32_t rawTemperature;
};
static struct gpu_sensor sensors[MAX_GPUS];
static unsigned sensorCount;
static NSString *reportPath;

static id property(io_registry_entry_t entry,CFStringRef name)
{
 return [(id)IORegistryEntryCreateCFProperty(entry,name,NULL,0) autorelease];
}
static NSString *textProperty(id value)
{
 if([value isKindOfClass:[NSString class]])return value;
 if([value isKindOfClass:[NSData class]]) {
  NSString *s=[[[NSString alloc] initWithData:value encoding:NSUTF8StringEncoding] autorelease];
  return [s stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\0"]];
 }
 return nil;
}
static void discover(void)
{
 io_iterator_t iterator=0;io_service_t service;
 if(IOServiceGetMatchingServices(kIOMasterPortDefault,IOServiceMatching("RadeonNI"),&iterator))return;
 while((service=IOIteratorNext(iterator))) {
  id head=property(service,CFSTR("IOFBDependentIndex"));
  if([head respondsToSelector:@selector(intValue)] && [head intValue]!=0) {IOObjectRelease(service);continue;}
  io_registry_entry_t pci=0;
  if(sensorCount==MAX_GPUS || IORegistryEntryGetParentEntry(service,kIOServicePlane,&pci)) {IOObjectRelease(service);continue;}
  if(!IOObjectConformsTo(pci,"IOPCIDevice")) {IOObjectRelease(pci);IOObjectRelease(service);continue;}
  struct gpu_sensor *g=&sensors[sensorCount++];g->framebuffer=service;
  NSString *model=textProperty(property(pci,CFSTR("model")));
  NSString *slot=textProperty(property(pci,CFSTR("AAPL,slot-name")));
  NSString *match=property(service,CFSTR("IOPCIMatch"));
  id device=property(pci,CFSTR("device-id"));
  uint32_t deviceID=0;
  if([device isKindOfClass:[NSData class]] && [device length]==sizeof(deviceID))
   [device getBytes:&deviceID];
  g->device=deviceID?deviceID:([match isKindOfClass:[NSString class]]?(strtoul([match UTF8String],NULL,0)>>16):0);
  id vram=property(pci,CFSTR("VRAM,totalsize"));
  if([vram isKindOfClass:[NSData class]] && [vram length]==sizeof(g->vramBytes))[vram getBytes:&g->vramBytes];
  g->name=[[NSString stringWithFormat:@"%@%@%@",model?model:@"Radeon GPU",slot?@" · ":@"",slot?slot:@""] copy];
  IOObjectRelease(pci);
 }
 IOObjectRelease(iterator);
}
static void connectSensor(struct gpu_sensor *g)
{
 if(g->connection)return;
 if(!IOServiceOpen(g->framebuffer,mach_task_self(),RDN_UC_TELEMETRY,&g->connection)) {
  g->telemetry=YES;return;
 }
 g->connection=0;g->telemetry=NO;
 NSString *path=property(g->framebuffer,CFSTR("IOAccelTypes"));
 if(![path isKindOfClass:[NSString class]])return;
 io_service_t service=IORegistryEntryFromPath(kIOMasterPortDefault,[path UTF8String]);
 if(!service)return;
 if(IOObjectConformsTo(service,RDN_UC_SERVICE_CLASS)) {
  if(IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE,&g->connection))g->connection=0;
  if(g->connection) {
   struct rdn_user_info info;IOByteCount bytes=sizeof(info);
   if(IOConnectMethodScalarIStructureO(g->connection,RDN_UC_GET_INFO,0,&bytes,&info) ||
      info.version!=RDN_USER_VERSION || info.pci_device_id!=g->device) {
    IOServiceClose(g->connection);g->connection=0;
   }
  }
 }
 IOObjectRelease(service);
}
static BOOL connectControl(struct gpu_sensor *g)
{
 if(g->control)return YES;
 NSString *path=property(g->framebuffer,CFSTR("IOAccelTypes"));
 if(![path isKindOfClass:[NSString class]])return NO;
 io_service_t service=IORegistryEntryFromPath(kIOMasterPortDefault,[path UTF8String]);
 if(!service)return NO;
 io_connect_t connection=0;struct rdn_user_info info;IOByteCount bytes=sizeof(info);
 BOOL ok=IOObjectConformsTo(service,RDN_UC_SERVICE_CLASS) &&
  !IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE_V2,&connection) &&
  !IOConnectMethodScalarIStructureO(connection,RDN_UC_GET_INFO,0,&bytes,&info) &&
  bytes==sizeof(info) && info.version==RDN_USER_VERSION && info.pci_device_id==g->device;
 if(!ok) {if(connection)IOServiceClose(connection);IOObjectRelease(service);return NO;}
 g->control=connection;g->accelerator=service;return YES;
}
static kern_return_t powerRequest(struct gpu_sensor *g,unsigned state,unsigned value)
{
 uint32_t core=0,memory=0,temperature=0;
 return IOConnectMethodScalarIScalarO(g->control,RDN_UC_POWER,2,3,state,value,&core,&memory,&temperature);
}
static NSTextField *label(NSRect frame,NSFont *font,NSColor *color)
{
 NSTextField *f=[[[NSTextField alloc] initWithFrame:frame] autorelease];
 [f setEditable:NO];[f setSelectable:NO];[f setBezeled:NO];[f setDrawsBackground:NO];
 [f setFont:font];[f setTextColor:color];[f setAlignment:NSCenterTextAlignment];return f;
}
@interface GPUTemperature : NSObject {
 NSWindow *panel;
 NSMutableArray *details;
 NSTabView *cards;
 NSMutableArray *rows,*controls,*modeLabels,*modeErrors,*thermalLimits;
 NSArray *lastValues;
}
- (void)pollSensors:(id)unused;
- (void)setPerformance:(NSButton *)sender;
- (void)setThermalLimit:(NSPopUpButton *)sender;
- (void)refreshControls;
- (void)copyReport:(id)sender;
- (NSString *)diagnosticReport;
- (void)saveReport:(id)sender;
- (void)refreshDetails;
- (void)showReadings:(NSArray *)values;
@end
@implementation GPUTemperature
- (void)applicationDidFinishLaunching:(NSNotification *)note
{
 discover();rows=[[NSMutableArray alloc] init];details=[[NSMutableArray alloc] init];
 thermalLimits=[[NSMutableArray alloc] init];controls=[[NSMutableArray alloc] init];modeLabels=[[NSMutableArray alloc] init];modeErrors=[[NSMutableArray alloc] init];
 panel=[[NSWindow alloc] initWithContentRect:NSMakeRect(100,100,940,660)
  styleMask:NSTitledWindowMask|NSClosableWindowMask|NSMiniaturizableWindowMask|NSResizableWindowMask backing:NSBackingStoreBuffered defer:NO];
 [panel setTitle:@"RadeonNI Driver Manager"];[panel setMinSize:NSMakeSize(940,660)];[panel center];
 NSMenu *menu=[[[NSMenu alloc] initWithTitle:@""] autorelease];
 NSMenuItem *app=[[[NSMenuItem alloc] initWithTitle:@"RadeonNI" action:NULL keyEquivalent:@""] autorelease];
 NSMenu *submenu=[[[NSMenu alloc] initWithTitle:@"RadeonNI"] autorelease];
 [submenu addItemWithTitle:@"Quit RadeonNI" action:@selector(terminate:) keyEquivalent:@"q"];
 [app setSubmenu:submenu];[menu addItem:app];[NSApp setMainMenu:menu];
 NSTextField *heading=label(NSMakeRect(24,612,650,30),[NSFont boldSystemFontOfSize:23],[NSColor controlTextColor]);
 [heading setAlignment:NSLeftTextAlignment];[heading setStringValue:@"RadeonNI Driver Manager"];
 [heading setAutoresizingMask:NSViewMinYMargin];[[panel contentView] addSubview:heading];
 NSButton *copy=[[[NSButton alloc] initWithFrame:NSMakeRect(730,610,186,30)] autorelease];
 [copy setTitle:@"Copy diagnostic report"];[copy setBezelStyle:NSRoundedBezelStyle];[copy setTarget:self];[copy setAction:@selector(copyReport:)];
 [copy setAutoresizingMask:NSViewMinXMargin|NSViewMinYMargin];[[panel contentView] addSubview:copy];
 cards=[[[NSTabView alloc] initWithFrame:NSMakeRect(16,16,908,580)] autorelease];
 [cards setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];[[panel contentView] addSubview:cards];
 unsigned count=sensorCount?sensorCount:1,i,j;
 for(i=0;i<count;i++) {
  NSTabViewItem *item=[[[NSTabViewItem alloc] initWithIdentifier:[NSNumber numberWithUnsignedInt:i]] autorelease];
  [item setLabel:sensorCount?sensors[i].name:@"No cards detected"];
  NSView *view=[[[NSView alloc] initWithFrame:NSMakeRect(0,0,880,540)] autorelease];[item setView:view];[cards addTabViewItem:item];
  NSMutableArray *row=[NSMutableArray array];
  const CGFloat positions[]={494,425,385,348,311,273};
  for(j=0;j<6;j++) {
   NSTextField *f=label(NSMakeRect(16,positions[j],338,j==1?56:j==5?48:32),
    j==1?[NSFont boldSystemFontOfSize:42]:[NSFont systemFontOfSize:j==0?14:13],
    [NSColor controlTextColor]);
   [f setAutoresizingMask:NSViewMinYMargin];
   [f setStringValue:j==0?(sensorCount?sensors[i].name:@"No RadeonNI cards found"):j==5?@"Connecting...":@"--"];
   [view addSubview:f];[row addObject:f];
  }
  [[row objectAtIndex:3] setToolTip:@"Time-weighted GUI_ACTIVE samples; short bursts may be missed."];
  [[row objectAtIndex:4] setToolTip:@"Memory-controller busy time includes display traffic; it is not bandwidth or VRAM capacity usage."];
  [rows addObject:row];
  NSButton *button=[[[NSButton alloc] initWithFrame:NSMakeRect(26,226,330,28)] autorelease];
  [button setButtonType:NSSwitchButton];[button setTitle:@"Performance Mode"];
  [button setTag:i];[button setTarget:self];[button setAction:@selector(setPerformance:)];
  [button setAutoresizingMask:NSViewMinYMargin];
  [button setToolTip:@"Requests the highest stock core state. Off restores automatic clocks. Both modes retain the selected temperature limit."];
  [view addSubview:button];[controls addObject:button];
  NSTextField *limitLabel=label(NSMakeRect(26,194,170,26),[NSFont systemFontOfSize:13],[NSColor controlTextColor]);
  [limitLabel setStringValue:@"Temperature limit:"];[limitLabel setAlignment:NSLeftTextAlignment];[limitLabel setAutoresizingMask:NSViewMinYMargin];[view addSubview:limitLabel];
  NSPopUpButton *limits=[[[NSPopUpButton alloc] initWithFrame:NSMakeRect(196,194,136,26) pullsDown:NO] autorelease];
  for(unsigned degrees=80;degrees<=100;degrees++){[limits addItemWithTitle:[NSString stringWithFormat:@"%u°C",degrees]];[[limits lastItem] setTag:degrees];}
  [limits selectItemAtIndex:10];[limits setTag:i];[limits setTarget:self];[limits setAction:@selector(setThermalLimit:)];[limits setAutoresizingMask:NSViewMinYMargin];
  [limits setToolTip:@"Stage 2 enters at this limit and recovers 5°C below it. Stage 1 recovers 10°C below the limit. Boot default: 90°C."];
  [view addSubview:limits];[thermalLimits addObject:limits];
  NSTextField *mode=label(NSMakeRect(16,132,338,60),[NSFont systemFontOfSize:12],[NSColor controlTextColor]);
  [mode setAutoresizingMask:NSViewMinYMargin];[view addSubview:mode];[modeLabels addObject:mode];[modeErrors addObject:@""];
  NSTextField *note=label(NSMakeRect(20,24,330,108),[NSFont systemFontOfSize:12],[NSColor disabledControlTextColor]);
  [note setStringValue:sensorCount && sensors[i].device==0x6704?@"Live measurements update once per second.\nBusy percentages are sampled activity.\nVoltage is the requested setting; power is an uncalibrated estimate.\nBoot starts automatic SMC when rdn_dpm=1.":@"Live measurements update once per second.\nBusy percentages are sampled activity.\nClocks are read back from hardware.\nRequested voltage is not a rail measurement.\nSMC power and thermal policy are not implemented for this card."];
  [view addSubview:note];
  NSTabView *sections=[[[NSTabView alloc] initWithFrame:NSMakeRect(374,24,490,500)] autorelease];
  [sections setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];[view addSubview:sections];
  NSMutableArray *texts=[NSMutableArray array];NSString *titles[]={@"Power & Health",@"Memory & Display",@"Diagnostics"};
  for(j=0;j<3;j++) {
   NSTabViewItem *section=[[[NSTabViewItem alloc] initWithIdentifier:titles[j]] autorelease];[section setLabel:titles[j]];
   NSScrollView *scroll=[[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,466,458)] autorelease];
   [scroll setHasVerticalScroller:YES];[scroll setBorderType:NSBezelBorder];[scroll setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];
   NSTextView *text=[[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,446,458)] autorelease];
   [text setEditable:NO];[text setFont:[NSFont userFixedPitchFontOfSize:12]];[text setTextContainerInset:NSMakeSize(12,12)];
   [text setVerticallyResizable:YES];[text setHorizontallyResizable:NO];[text setAutoresizingMask:NSViewWidthSizable];
   [[text textContainer] setWidthTracksTextView:YES];[scroll setDocumentView:text];[section setView:scroll];[sections addTabViewItem:section];[texts addObject:text];
  }
  [details addObject:texts];
 }
 [self refreshControls];[self refreshDetails];[panel makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
 [NSThread detachNewThreadSelector:@selector(pollSensors:) toTarget:self withObject:nil];
 if(reportPath)[NSTimer scheduledTimerWithTimeInterval:3 target:self selector:@selector(saveReport:) userInfo:nil repeats:NO];
}
- (void)refreshDetails
{
 unsigned i;
 for(i=0;i<sensorCount;i++) {
  struct gpu_sensor *g=&sensors[i];BOOL connected=connectControl(g);
  NSMutableString *report=[NSMutableString stringWithFormat:@"DEVICE\n%@\nPCI device: %04x\nInstalled VRAM: %.0f MiB\n\n",g->name,g->device,g->vramBytes/1048576.0];
  struct rdn_user_info info;IOByteCount bytes=sizeof(info);
  if(connected && !IOConnectMethodScalarIStructureO(g->control,RDN_UC_GET_INFO,0,&bytes,&info)) {
   [report appendFormat:@"MEMORY AND RENDERING\nCPU-visible aperture: %.0f MiB\nVisible-VRAM heap capacity: %.0f MiB\nRender backends: %u   Shader engines: %u\nTile pipes: %u\n\nFRAMEBUFFER\n%u x %u, %u bits/pixel\nPitch: %u pixels\n\n",info.aperture_size/1048576.0,info.heap_size/1048576.0,info.max_backends,info.num_ses,info.max_tile_pipes,info.fb_width,info.fb_height,info.fb_bits_per_pixel,info.fb_pitch_pixels];
  }
  if(connected) {
   struct rdn_user_memory_usage usage; IOByteCount usageBytes=sizeof(usage);
   kern_return_t usageError=IOConnectMethodScalarIStructureO(g->control,RDN_UC_MEMORY_USAGE,0,&usageBytes,&usage);
   if(!usageError && usageBytes==sizeof(usage) && usage.version==1 && usage.size==sizeof(usage)) {
    double total=memoryValue(usage.total),freeBytes=memoryValue(usage.free);
    [report appendFormat:@"GLOBAL VRAM (all clients)\nOccupied: %.1f / %.1f MiB (%.2f%%)\nDynamic allocations: %.1f MiB\nReserved/unavailable: %.1f MiB\nFree: %.1f MiB\nVisible free / largest block: %.1f / %.1f MiB\nHidden free / largest block: %.1f / %.1f MiB\nCommand reserve free: %.1f / %.1f MiB\n\n",(total-freeBytes)/1048576.0,total/1048576.0,total?100.0*(total-freeBytes)/total:0,memoryValue(usage.allocated)/1048576.0,memoryValue(usage.reserved)/1048576.0,freeBytes/1048576.0,memoryValue(usage.visible_free)/1048576.0,memoryValue(usage.visible_largest)/1048576.0,memoryValue(usage.hidden_free)/1048576.0,memoryValue(usage.hidden_largest)/1048576.0,memoryValue(usage.command_free)/1048576.0,memoryValue(usage.command_capacity)/1048576.0];
   } else [report appendFormat:@"Global VRAM occupancy: unavailable (query 0x%08x; requires memory-usage driver)\n",usageError];
   unsigned offset=0,hidden=0,enabled=0,base=0,pages=0;
   kern_return_t e=IOConnectMethodScalarIScalarO(g->control,RDN_UC_HIDDEN_INFO,0,2,&offset,&hidden);
   if(!e)[report appendFormat:@"Hidden-VRAM heap capacity: %.1f MiB\n",hidden/1048576.0];
   else [report appendFormat:@"Hidden-VRAM query failed: 0x%08x\n",e];
   e=IOConnectMethodScalarIScalarO(g->control,RDN_UC_GART_INFO,0,3,&enabled,&base,&pages);
   if(!e)[report appendFormat:@"GART enabled: %@\nGART address-space capacity: %.1f MiB (not resident RAM)\n\n",enabled?@"Yes":@"No",pages*4096.0/1048576.0];
   else [report appendFormat:@"GART query failed: 0x%08x\n",e];
  }
  [report appendString:@"POWER AND HEALTH\n"];
  const char *keys[]={"HardwarePowerManagement","HardwareThermalProtection","HardwarePowerContainment","PerformanceMode","ThermalLimitC","ThermalFallback","ManualPowerCap","HardwarePowerFault","HardwarePowerError","PowerStateIndex","PowerCoreVoltageMV","EstimatedPowerMilliWatts","PowerLimitWatts","NearPowerLimitWatts","UndervoltActive","UndervoltRequestedMV","ThermalStage","CoolingCoreMHz","IntermediateCoreMHz"};
  NSString *names[]={@"SMC management",@"Hardware thermal protection",@"Hardware power containment",@"Performance requested",@"Temperature limit (°C)",@"Thermal clock reduction",@"Manual clock cap",@"Power fault",@"Power error code",@"SMC state index",@"Requested core voltage (mV)",@"SMC power estimate (mW; uncalibrated)",@"VBIOS SMC TDP target (W)",@"VBIOS SMC near-TDP target (W)",@"Temporary undervolt active",@"Last voltage request (mV)",@"Thermal stage (0 / 1 / 2)",@"Cooling core target (MHz)",@"Intermediate core target (MHz)"};
  unsigned k;for(k=0;k<sizeof(keys)/sizeof(keys[0]);k++) {
   if(g->device!=0x6704 && k!=10)continue;
   id value=connected?property(g->accelerator,(CFStringRef)[NSString stringWithUTF8String:keys[k]]):nil;
   if(k==10 && connected && ![property(g->accelerator,CFSTR("IdleGovernor")) boolValue]) {
    id requested=property(g->accelerator,CFSTR("RequestedCoreVoltageMV"));
    if(requested)value=requested;
   }
   [report appendFormat:@"%@: %@\n",names[k],value?((k<4 || k==5 || k==6 || k==7 || k==14)?([value boolValue]?@"Yes":@"No"):k==8?[NSString stringWithFormat:@"%d (0x%08x)",[value intValue],[value unsignedIntValue]]:[value description]):g->device!=0x6704?@"Not exposed by this card’s driver policy":@"Not reported by driver"];
  }
  const char *commonKeys[]={"RequestedIOVoltageMV","PowerCoreClock10KHz","PowerMemoryClock10KHz","IdleGovernor","PowerIdle","IdleGovernorError","ClockTuningActive","ClockTuningRestoreError"};
  NSString *commonNames[]={@"Requested I/O voltage (mV; 0 means unchanged)",@"Requested core clock (10 kHz units)",@"Requested memory clock (10 kHz units)",@"Software idle governor",@"Software idle state active",@"Idle-governor error",@"Temporary clock tuning active",@"Clock-tuning restore error"};
  for(unsigned q=0;q<sizeof(commonKeys)/sizeof(commonKeys[0]);q++) {
   id value=connected?property(g->accelerator,(CFStringRef)[NSString stringWithUTF8String:commonKeys[q]]):nil;
   if(value)[report appendFormat:@"%@: %@\n",commonNames[q],(q==3 || q==4 || q==6)?([value boolValue]?@"Yes":@"No"):[value description]];
  }
  const char *policyKeys[]={"MemoryDPM","PowerPlayIdle","BIOSVideoStateActive","BIOSULVStateSupported","GraphicsClockGating","MediumGrainClockGating","DynamicFanControl","PowerPlayContextError","FanControlError","MemorySwitchBlankingUS"};
  NSString *policyNames[]={@"Memory DPM allowed by displays",@"BIOS idle cap active",@"BIOS video state active",@"BIOS ULV state available",@"Graphics clock gating",@"Medium-grain clock gating",@"Dynamic fan policy",@"PowerPlay context error",@"Fan-control error",@"Minimum display blanking (µs)"};
  for(unsigned q=0;g->device==0x6704 && q<sizeof(policyKeys)/sizeof(policyKeys[0]);q++) {
   id value=connected?property(g->accelerator,(CFStringRef)[NSString stringWithUTF8String:policyKeys[q]]):nil;
   if(!value)[report appendFormat:@"%@: Not reported by driver\n",policyNames[q]];
   else if(q<7)[report appendFormat:@"%@: %@\n",policyNames[q],[value boolValue]?@"Yes":@"No"];
   else if(q<9)[report appendFormat:@"%@: %d (0x%08x)\n",policyNames[q],[value intValue],[value unsignedIntValue]];
   else [report appendFormat:@"%@: %@\n",policyNames[q],value];
  }
  id fan=property(g->framebuffer,CFSTR("FanRequestedPercent"));
  [report appendFormat:@"Requested fan duty: %@%@\n",fan?fan:@"Unavailable",fan?@"%":@""];
  struct rdn_user_board_sensors board; IOByteCount boardBytes=sizeof(board);
  kern_return_t boardError=kIOReturnUnsupported;
  if(g->connection && g->telemetry)boardError=IOConnectMethodScalarIStructureO(g->connection,RDN_TELEMETRY_BOARD,0,&boardBytes,&board);
  if(!boardError && boardBytes==sizeof(board) && board.version==1 && board.size==sizeof(board)) {
   if(board.fan_status==RDN_SENSOR_VALID)[report appendFormat:@"Measured fan speed: %u RPM\n",board.fan_rpm];
   else [report appendFormat:@"Measured fan speed: %@\n",sensorStatus(board.fan_status)];
   if(board.fan_pulses)[report appendFormat:@"Tach period: %u; reference: %u Hz\nBIOS pulses/revolution: %u; configured edges: %u\nBIOS fan range: %u–%u RPM\n",board.tach_period,board.tach_clock_hz,board.fan_pulses,board.tach_edges,board.fan_min_rpm,board.fan_max_rpm];
   if(board.rail_status==RDN_SENSOR_VALID)[report appendFormat:@"Measured core rail: %.3f V\n",board.rail_microvolts/1000000.0];
   else [report appendFormat:@"Measured core rail: %@\n",sensorStatus(board.rail_status)];
   [report appendFormat:@"BIOS voltage control ID: 0x%x; thermal controller: %u\n",board.voltage_control_id,board.thermal_controller];
  } else [report appendFormat:@"Board sensors unavailable (query 0x%08x; requires board-sensor driver)\n",boardError];
  [report appendString:@"\nDISPLAY AND DRIVER\n"];
  const char *fbkeys[]={"HardwareGLActive","DisplayHeadCount","IOFBCurrentPixelClock"};
  NSString *fbnames[]={@"Hardware GL active",@"Available display heads (not connected monitors)",@"Current pixel clock (Hz)"};
  for(k=0;k<3;k++){id value=property(g->framebuffer,(CFStringRef)[NSString stringWithUTF8String:fbkeys[k]]);[report appendFormat:@"%@: %@\n",fbnames[k],value?value:@"Unavailable"];}
  const char *akeys[]={"IOGLBundleName","GLDDeviceIndex","RadeonNISurfaceGPUCopies","RadeonNISurfaceGPUBytes","RadeonNISurfaceGPURebootRequired"};
  NSString *anames[]={@"OpenGL bundle",@"GL device index",@"GPU surface copies (total)",@"GPU surface bytes (total)",@"Surface restart required"};
  for(k=0;k<5;k++){id value=connected?property(g->accelerator,(CFStringRef)[NSString stringWithUTF8String:akeys[k]]):nil;[report appendFormat:@"%@: %@\n",anames[k],value?value:@"Unavailable"];}
  id copied=connected?property(g->accelerator,CFSTR("RadeonNISurfaceGPUBytes")):nil;
  uint64_t now=mach_absolute_time();mach_timebase_info_data_t tb;mach_timebase_info(&tb);
  if(copied && g->copyTime && [copied unsignedLongLongValue]>=g->copyBytes) {
   double elapsed=(now-g->copyTime)*(double)tb.numer/tb.denom/1e9;
   if(elapsed>0)[report appendFormat:@"Surface-copy rate: %.2f MiB/s\n",([copied unsignedLongLongValue]-g->copyBytes)/1048576.0/elapsed];
  }
  g->copyBytes=[copied unsignedLongLongValue];g->copyTime=now;
  if(connected) {
   unsigned irq=0,events=0,fallbacks=0,up=0,vcpu=0,ring=0;
   kern_return_t e=IOConnectMethodScalarIScalarO(g->control,RDN_UC_FENCE_CONTROL,1,3,0,&irq,&events,&fallbacks);
   if(!e)[report appendFormat:@"Fence completion: %@\nFence IRQ events: %u\nFence polling fallbacks: %u\n",(irq&1)?@"Interrupts enabled":@"Polling",events,fallbacks];
   else [report appendFormat:@"Fence status query failed: 0x%08x\n",e];
   e=IOConnectMethodScalarIScalarO(g->control,RDN_UC_UVD_INFO,0,3,&up,&vcpu,&ring);
   if(!e)[report appendFormat:@"UVD engine initialized: %@\n",up?@"Yes":@"No"];
   else [report appendFormat:@"UVD state query failed: 0x%08x\n",e];
   struct rdn_user_uvd_diagnostic uvd;IOByteCount n=sizeof(uvd);
   e=IOConnectMethodScalarIStructureO(g->control,RDN_UC_UVD_DIAGNOSTIC_INFO,0,&n,&uvd);
   if(!e && n==sizeof(uvd) && uvd.version==RDN_UVD_DIAGNOSTIC_VERSION)
    [report appendFormat:@"UVD firmware present: %@\nUVD diagnostic state: %u (0 off, 1 ready, 2 faulted)\nUVD last error: %d\n",(uvd.flags&RDN_UVD_DIAGNOSTIC_FIRMWARE)?@"Yes":@"No",uvd.state,uvd.last_error];
   else [report appendFormat:@"UVD diagnostic query unavailable: 0x%08x\n",e];
   io_string_t path;if(!IORegistryEntryGetPath(g->accelerator,kIOServicePlane,path))
    [report appendFormat:@"\nAccelerator: %s\n",path];
  }
  [report appendString:@"\nUVD initialization does not establish active video decoding.\nNo decoder utilization counter is exposed.\nFan duty is requested PWM, not measured RPM.\nSMC power is uncalibrated; it is not measured board/slot power.\nMeasured rail voltage is available only when the sensor backend reports valid data.\n"];
  NSUInteger power=[report rangeOfString:@"POWER AND HEALTH\n"].location;
  NSUInteger driver=[report rangeOfString:@"DISPLAY AND DRIVER\n"].location;
  NSArray *parts=[NSArray arrayWithObjects:[report substringWithRange:NSMakeRange(power,driver-power)],
   [report substringToIndex:power],[report substringFromIndex:driver],nil];
  for(k=0;k<3;k++){NSTextView *text=[[details objectAtIndex:i] objectAtIndex:k];NSString *part=[parts objectAtIndex:k];if(![[text string] isEqualToString:part])[text setString:part];}
 }
}
- (NSString *)diagnosticReport
{
 NSMutableString *report=[NSMutableString stringWithFormat:@"RadeonNI Driver Manager — %@\n",[NSDate date]];unsigned i;
 for(i=0;i<sensorCount;i++) {
  [report appendFormat:@"%@\n",sensors[i].name];
  NSArray *row=[rows objectAtIndex:i];unsigned j;for(j=1;j<6;j++)[report appendFormat:@"%@\n",[[row objectAtIndex:j] stringValue]];
  [report appendFormat:@"%@\n%@\n",[[modeLabels objectAtIndex:i] stringValue],@""];
  for(j=0;j<3;j++)[report appendFormat:@"%@\n",[[[details objectAtIndex:i] objectAtIndex:j] string]];
 }
 return report;
}
- (void)saveReport:(id)sender
{
 NSError *error=nil;
 if(![[self diagnosticReport] writeToFile:reportPath atomically:YES encoding:NSUTF8StringEncoding error:&error])NSLog(@"Report write failed: %@",error);
 [NSApp terminate:nil];
}
- (void)copyReport:(id)sender
{
 NSString *report=[self diagnosticReport];
 NSPasteboard *pasteboard=[NSPasteboard generalPasteboard];[pasteboard declareTypes:[NSArray arrayWithObject:NSStringPboardType] owner:nil];[pasteboard setString:report forType:NSStringPboardType];
}

- (void)refreshControls
{
 unsigned i;
 for(i=0;i<[controls count];i++) {
  NSButton *button=[controls objectAtIndex:i];NSTextField *label=[modeLabels objectAtIndex:i];
  NSPopUpButton *limits=[thermalLimits objectAtIndex:i];[limits setEnabled:NO];
  if(i>=sensorCount) {[button setEnabled:NO];continue;}
  if(sensors[i].device!=0x6704) {
   struct gpu_sensor *g=&sensors[i];BOOL connected=connectControl(g);
   id governor=connected?property(g->accelerator,CFSTR("IdleGovernor")):nil;
   BOOL idle=[property(g->accelerator,CFSTR("PowerIdle")) boolValue];
   [button setEnabled:NO];[button setState:NSOffState];
   [label setStringValue:!connected?@"Power-policy connection unavailable":[governor boolValue]?(idle?@"Software governor · idle state":@"Software governor · active state"):@"Fixed BIOS clock state\nSMC performance and thermal controls unavailable"];
   continue;
  }
  struct gpu_sensor *g=&sensors[i];BOOL connected=connectControl(g);
  BOOL active=connected && [property(g->accelerator,CFSTR("HardwarePowerManagement")) boolValue];
  BOOL supported=connected && [property(g->accelerator,CFSTR("PerformanceModeSupported")) boolValue];
  BOOL performance=connected && [property(g->accelerator,CFSTR("PerformanceMode")) boolValue];
  BOOL fault=connected && [property(g->accelerator,CFSTR("HardwarePowerFault")) boolValue];
  unsigned limit=connected?[property(g->accelerator,CFSTR("ThermalLimitC")) unsignedIntValue]:0;
  unsigned maxLimit=connected?[property(g->accelerator,CFSTR("ThermalLimitMaxC")) unsignedIntValue]:0;
  [limits setEnabled:active && !fault && maxLimit==100];
  if(limit>=80 && limit<=100)[limits selectItemAtIndex:limit-80];
  BOOL cooling=connected && [property(g->accelerator,CFSTR("ThermalFallback")) boolValue];
  unsigned stage=connected?[property(g->accelerator,CFSTR("ThermalStage")) unsignedIntValue]:0;
  BOOL capped=connected && [property(g->accelerator,CFSTR("ManualPowerCap")) boolValue];
  [button setEnabled:connected && !fault && (!active || supported)];[button setState:active && performance?NSOnState:NSOffState];
  NSString *error=[modeErrors objectAtIndex:i];
  [label setStringValue:[error length]?error:!connected?@"Power-control connection unavailable":fault?@"Power-management fault — restart required":!active?@"Enable to start guarded power management":!supported?@"Driver update required for performance control":
   [NSString stringWithFormat:@"%@ · %u°C limit · %@",cooling?(stage==1?@"Warm — intermediate clock":@"Cooling — lowest clock"):capped?@"Low-clock cap":performance?@"Performance":@"Automatic",limit,cooling?@"thermal reduction active":@"no thermal reduction"]];
 }
}
- (void)setPerformance:(NSButton *)sender
{
 unsigned i=(unsigned)[sender tag];if(i>=sensorCount)return;
 struct gpu_sensor *g=&sensors[i];BOOL on=[sender state]==NSOnState;
 if(g->device!=0x6704 || !connectControl(g))return;
 [sender setEnabled:NO];
 BOOL active=[property(g->accelerator,CFSTR("HardwarePowerManagement")) boolValue];
 kern_return_t err=0;
 if(on && !active)err=powerRequest(g,RDN_UC_POWER_START_SMC,0);
 if(!err)err=powerRequest(g,RDN_UC_POWER_PROFILE,on?1:0);
 NSString *message=err?[NSString stringWithFormat:@"Mode change failed: %08x",err]:@"";
 [modeErrors replaceObjectAtIndex:i withObject:message];
 NSLog(@"GPU %@ performance=%d result=%08x",g->name,on,err);
 [self refreshControls];
}
- (void)setThermalLimit:(NSPopUpButton *)sender
{
 unsigned i=(unsigned)[sender tag],degrees=(unsigned)[[sender selectedItem] tag];
 if(i>=sensorCount || degrees<80 || degrees>100)return;
 struct gpu_sensor *g=&sensors[i];if(!connectControl(g))return;
 [sender setEnabled:NO];kern_return_t err=powerRequest(g,RDN_UC_POWER_THERMAL_LIMIT,degrees);
 [modeErrors replaceObjectAtIndex:i withObject:err?[NSString stringWithFormat:@"Temperature change failed: %08x",err]:@""];
 [self refreshControls];[self refreshDetails];
}
- (void)pollSensors:(id)unused
{
 mach_timebase_info_data_t tb;mach_timebase_info(&tb);unsigned reports=0;
 for(;;) {
  NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];
  unsigned i,seed=(unsigned)mach_absolute_time();
  for(i=0;i<sensorCount;i++) {
   struct gpu_sensor *g=&sensors[i];connectSensor(g);
   g->total=g->core=g->memory=g->nonDisplay=0;g->samples=g->attempts=0;g->havePrevious=NO;
  }
  uint64_t start=mach_absolute_time();
  do {
   for(i=0;i<sensorCount;i++) {
    struct gpu_sensor *g=&sensors[i];uint32_t core=0,mem=0;
    if(!g->connection)continue;
    kern_return_t err;
    if(g->telemetry)err=IOConnectMethodScalarIScalarO(g->connection,RDN_TELEMETRY_SAMPLE,0,3,&g->rawTemperature,&core,&mem);
    else {
     err=IOConnectMethodScalarIScalarO(g->connection,RDN_UC_REG_READ,1,1,0x8010,&core);
     if(!err)err=IOConnectMethodScalarIScalarO(g->connection,RDN_UC_REG_READ,1,1,0xe50,&mem);
    }
    g->attempts++;BOOL ok=err==KERN_SUCCESS;
    uint64_t now=mach_absolute_time();
    if(ok && g->havePrevious) {
     uint64_t dt=now-g->previous;g->total+=dt;
     if(g->oldCore&0x80000000u)g->core+=dt;
     if(g->oldMemory&0x1e00u)g->memory+=dt;
     if(g->oldMemory&0x400u)g->nonDisplay+=dt;
    }
    g->previous=now;g->oldCore=core;g->oldMemory=mem;g->havePrevious=ok;
    if(ok)g->samples++;else if(err!=kIOReturnBusy) {IOServiceClose(g->connection);g->connection=0;g->total=0;}
   }
   seed=seed*1664525u+1013904223u;usleep(650+seed%501);
  }while((mach_absolute_time()-start)*tb.numer/tb.denom<1000000000ull);
  NSMutableArray *values=[NSMutableArray array];
  for(i=0;i<sensorCount;i++) {
   struct gpu_sensor *g=&sensors[i];uint32_t raw=0,sc=0,mc=0,biased=0;int temperature=-1000000;
   raw=g->rawTemperature;
   if(g->connection && (g->telemetry?g->samples>0:!IOConnectMethodScalarIScalarO(g->connection,RDN_UC_REG_READ,1,1,0x740,&raw))) {
    unsigned t=(raw>>16)&0x7ff;
    if(!(t&0x600))temperature=((t&0x100)?(int)(t&0x1ff)-0x200:(int)(t&0xff))*500;
   }
   if(g->connection) {
    kern_return_t err=g->telemetry?IOConnectMethodScalarIScalarO(g->connection,RDN_TELEMETRY_CLOCKS,0,2,&sc,&mc):
     IOConnectMethodScalarIScalarO(g->connection,RDN_UC_POWER,2,3,RDN_UC_POWER_QUERY,0,&sc,&mc,&biased);
    if(err)sc=mc=0;
   }
   NSArray *v=[NSArray arrayWithObjects:[NSNumber numberWithInt:temperature],
    [NSNumber numberWithInt:g->total?(int)((g->core*1000+g->total/2)/g->total):-1],
    [NSNumber numberWithInt:g->total?(int)((g->memory*1000+g->total/2)/g->total):-1],
    [NSNumber numberWithUnsignedInt:sc],[NSNumber numberWithUnsignedInt:mc],
    [NSNumber numberWithBool:g->connection!=0],
    [NSNumber numberWithUnsignedInt:g->samples],[NSNumber numberWithUnsignedInt:g->attempts],
    [NSNumber numberWithInt:g->total?(int)((g->nonDisplay*1000+g->total/2)/g->total):-1],
    [NSNumber numberWithDouble:g->total*(double)tb.numer/tb.denom/1e6],nil];
   [values addObject:v];
   if(reports%60==0)NSLog(@"GPU %@ device=%04x connected=%d samples=%u readings=%@",g->name,g->device,g->connection!=0,g->samples,v);
  }
  reports++;[self performSelectorOnMainThread:@selector(showReadings:) withObject:values waitUntilDone:YES];
  [pool drain];
 }
}
- (void)showReadings:(NSArray *)values
{
 [self refreshControls];[self refreshDetails];
 if([lastValues isEqualToArray:values])return;
 [lastValues release];lastValues=[values copy];unsigned i;
 for(i=0;i<[values count];i++) {
  NSArray *v=[values objectAtIndex:i],*row=[rows objectAtIndex:i];
  int temp=[[v objectAtIndex:0] intValue],core=[[v objectAtIndex:1] intValue],mem=[[v objectAtIndex:2] intValue];
  unsigned sc=[[v objectAtIndex:3] unsignedIntValue],mc=[[v objectAtIndex:4] unsignedIntValue];
  BOOL connected=[[v objectAtIndex:5] boolValue];
  [[row objectAtIndex:1] setStringValue:temp==-1000000?@"--":[NSString stringWithFormat:@"%.1f %CC",temp/1000.0,(unichar)0x00b0]];
  [[row objectAtIndex:1] setTextColor:temp>=90000?[NSColor redColor]:temp>=80000?[NSColor orangeColor]:[NSColor controlTextColor]];
  [[row objectAtIndex:2] setStringValue:sc&&mc?[NSString stringWithFormat:@"Core %.2f / Memory %.2f MHz",sc/100.0,mc/100.0]:@"Clocks unavailable"];
  [[row objectAtIndex:3] setStringValue:core<0?@"Graphics engine busy: --":[NSString stringWithFormat:@"Graphics engine busy: %.1f%%",core/10.0]];
  [[row objectAtIndex:4] setStringValue:mem<0?@"Memory controller busy: --":[NSString stringWithFormat:@"Memory controller busy: %.1f%%",mem/10.0]];
  [[row objectAtIndex:5] setStringValue:!connected?@"Telemetry unavailable — no driver connection":temp==-1000000?@"Temperature sensor unavailable":[NSString stringWithFormat:@"%u/%u samples · %.0f ms covered\nNon-display MC busy: %@",[[v objectAtIndex:6] unsignedIntValue],[[v objectAtIndex:7] unsignedIntValue],[[v objectAtIndex:9] doubleValue],[[v objectAtIndex:8] intValue]<0?@"unavailable":[NSString stringWithFormat:@"%.1f%%",[[v objectAtIndex:8] intValue]/10.0]]];
 }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {return YES;}
@end
int main(int argc,char **argv)
{
 NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];[NSApplication sharedApplication];
 if(argc==3 && !strcmp(argv[1],"--report"))reportPath=[[NSString stringWithUTF8String:argv[2]] copy];
 GPUTemperature *delegate=[[GPUTemperature alloc] init];[NSApp setDelegate:delegate];
 [NSApp run];[delegate release];[pool drain];return 0;
}
