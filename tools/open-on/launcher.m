#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "gpus.h"
static int fail(NSString *message)
{
 const char *trace=getenv("RDN_OPEN_ON_TRACE");if(trace){FILE *f=fopen(trace,"a");if(f){fprintf(f,"error=%s\n",[message UTF8String]);fclose(f);}}
 [NSApplication sharedApplication];[NSApp activateIgnoringOtherApps:YES];
 NSRunAlertPanel(@"Open on GPU",@"%@",@"OK",nil,nil,message);return 1;
}
int main(int argc,char **argv)
{
 NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];
 const char *trace=getenv("RDN_OPEN_ON_TRACE");if(trace){FILE *f=fopen(trace,"a");if(f){fprintf(f,"argc=%d\n",argc);for(int i=0;i<argc;i++)fprintf(f,"argv[%d]=%s\n",i,argv[i]);fclose(f);}}
 int first=1;while(first<argc && !strncmp(argv[first],"-psn_",5))first++;
 if(argc-first!=2){[pool drain];return 1;}
 NSString *path=[[NSString stringWithUTF8String:argv[first]] stringByResolvingSymlinksInPath];
 char *end;long index=strtol(argv[first+1],&end,10);
 if(*end || index<0 || index>3)return fail(@"The selected GPU is invalid.");
 NSBundle *bundle=[NSBundle bundleWithPath:path];
 if(![[path pathExtension] isEqualToString:@"app"]||!bundle||![bundle executablePath])return fail(@"Select an application bundle in Finder.");
 struct open_gpu gpu[4];unsigned count=open_gpus(gpu,4),selected;
 for(selected=0;selected<count;selected++)if(gpu[selected].index==index)break;
 if(selected==count || !gpu[selected].ready){free_gpus(gpu,count);return fail(@"The selected GPU is unavailable.");}
 NSString *identifier=[bundle bundleIdentifier];
 for(NSDictionary *app in [[NSWorkspace sharedWorkspace] launchedApplications]) {
  NSString *running=[[app objectForKey:@"NSApplicationPath"] stringByResolvingSymlinksInPath];
  if([running isEqualToString:path] || (identifier && [[app objectForKey:@"NSApplicationBundleIdentifier"] isEqualToString:identifier])) {
   NSString *message=[NSString stringWithFormat:@"%@ is already running. Quit it, then use Open on to choose its GPU.",[[NSFileManager defaultManager] displayNameAtPath:path]];
   free_gpus(gpu,count);return fail(message);
  }
 }
 [NSApplication sharedApplication];
 GLuint mask=0;
 for(NSScreen *screen in [NSScreen screens]) {
  CGDirectDisplayID display=[[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
  io_service_t framebuffer=CGDisplayIOServicePort(display);io_name_t plane=kIOServicePlane;
  CFTypeRef owner=IORegistryEntrySearchCFProperty(framebuffer,plane,CFSTR("GLDDeviceIndex"),NULL,kIORegistryIterateRecursively|kIORegistryIterateParents);
  int gpuIndex=-1;if(owner && CFGetTypeID(owner)==CFNumberGetTypeID())CFNumberGetValue(owner,kCFNumberIntType,&gpuIndex);
  if(owner)CFRelease(owner);
  if(trace){FILE *f=fopen(trace,"a");if(f){fprintf(f,"display=%u gpu=%d\n",display,gpuIndex);fclose(f);}}
  if(gpuIndex==index)mask|=CGDisplayIDToOpenGLDisplayMask(display);
 }
 if(!mask){free_gpus(gpu,count);return fail(@"Connect a display to the selected GPU before opening an application on it.");}
 NSString *shim=@"/Library/Application Support/RadeonNI/OpenOn.app/Contents/Frameworks/GPUSelect.dylib";
 if(![[NSFileManager defaultManager] fileExistsAtPath:shim]){free_gpus(gpu,count);return fail(@"The GPU launcher is incomplete. Reinstall RadeonNI Open on.");}
 NSMutableDictionary *environment=[NSMutableDictionary dictionary];
 NSDictionary *configured=[bundle objectForInfoDictionaryKey:@"LSEnvironment"];
 NSString *previous=[configured isKindOfClass:[NSDictionary class]]?[configured objectForKey:@"DYLD_INSERT_LIBRARIES"]:nil;
 [environment setObject:previous?[shim stringByAppendingFormat:@":%@",previous]:shim forKey:@"DYLD_INSERT_LIBRARIES"];
 [environment setObject:[NSString stringWithFormat:@"%ld",index] forKey:@"RDN_OPEN_ON_GPU_INDEX"];
 [environment setObject:[NSString stringWithFormat:@"%u",(unsigned)mask] forKey:@"RDN_FORCE_DISPLAY_MASK"];
 [environment setObject:@"0" forKey:@"RDN_GPU_SWITCH"];
 FSRef app;OSStatus error=FSPathMakeRef((const UInt8 *)[path fileSystemRepresentation],&app,NULL);
 if(!error) {
  LSApplicationParameters parameters={0};parameters.application=&app;parameters.environment=(CFDictionaryRef)environment;
  parameters.flags=kLSLaunchNewInstance;
  error=LSOpenApplication(&parameters,NULL);
  if(trace){FILE *f=fopen(trace,"a");if(f){fprintf(f,"gpu=%ld mask=%x launch=%d\n",index,mask,(int)error);fclose(f);}}
 }
 free_gpus(gpu,count);
 if(error)return fail([NSString stringWithFormat:@"The application could not be opened (error %d).",(int)error]);
 [pool drain];return 0;
}
