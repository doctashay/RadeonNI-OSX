/* Leopard Finder contextual-menu interface. */
#include <Carbon/Carbon.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <stdlib.h>
#include "gpus.h"
#define FACTORY CFSTR("E6A8B5D8-5F4A-4BA9-9EA6-D5292DCD49A1")
struct menu_instance {
 ContextualMenuInterfaceStruct *interface;
 CFUUIDRef factory;UInt32 refs;
 CFURLRef application;struct open_gpu gpu[4];unsigned count;
};
static void cleanup(void *object)
{
 struct menu_instance *m=object;
 if(m->application)CFRelease(m->application);m->application=NULL;
 free_gpus(m->gpu,m->count);m->count=0;
}
static UInt32 retain_instance(void *object){return ++((struct menu_instance *)object)->refs;}
static UInt32 release_instance(void *object)
{
 struct menu_instance *m=object;UInt32 refs=--m->refs;
 if(!refs){cleanup(m);CFPlugInRemoveInstanceForFactory(m->factory);CFRelease(m->factory);free(m);}return refs;
}
static SInt32 query(void *object,CFUUIDBytes bytes,void **result)
{
 if(!result)return E_POINTER;*result=NULL;
 CFUUIDRef id=CFUUIDCreateFromUUIDBytes(NULL,bytes);
 int matches=CFEqual(id,kContextualMenuInterfaceID)||CFEqual(id,IUnknownUUID);CFRelease(id);
 if(!matches)return E_NOINTERFACE;
 retain_instance(object);*result=object;return S_OK;
}
static OSStatus item_name(AERecord *record,CFStringRef text)
{
 UniChar chars[512];CFIndex n=CFStringGetLength(text);if(n>512)n=512;
 CFStringGetCharacters(text,CFRangeMake(0,n),chars);
 return AEPutKeyPtr(record,keyContextualMenuName,typeUnicodeText,chars,n*sizeof(UniChar));
}
static OSStatus examine(void *object,const AEDesc *context,AEDescList *commands)
{
 struct menu_instance *m=object;long count=0;FSRef ref;Size size=0;OSStatus err;
 cleanup(m);
 if(!context || AECountItems(context,&count) || count!=1)return noErr;
 if(AEGetNthPtr(context,1,typeFSRef,NULL,NULL,&ref,sizeof(ref),&size))return noErr;
 LSItemInfoRecord info;
 if(LSCopyItemInfoForRef(&ref,kLSRequestBasicFlagsOnly,&info)||!(info.flags&kLSItemInfoIsApplication))return noErr;
 m->application=CFURLCreateFromFSRef(NULL,&ref);
 if(!m->application)return noErr;
 CFStringRef extension=CFURLCopyPathExtension(m->application);
 int app=extension&&CFStringCompare(extension,CFSTR("app"),kCFCompareCaseInsensitive)==kCFCompareEqualTo;
 if(extension)CFRelease(extension);
 if(!app){cleanup(m);return noErr;}
 m->count=open_gpus(m->gpu,4);if(!m->count)return noErr;
 AEDescList submenu={typeNull,NULL};AERecord parent={typeNull,NULL};
 err=AECreateList(NULL,0,false,&submenu);
 for(unsigned i=0;!err && i<m->count;i++) {
  AERecord child={typeNull,NULL};err=AECreateList(NULL,0,true,&child);
  if(!err)err=item_name(&child,m->gpu[i].name);
  SInt32 command=100+m->gpu[i].index;
  if(!err)err=AEPutKeyPtr(&child,keyContextualMenuCommandID,typeSInt32,&command,sizeof(command));
  if(!err && !m->gpu[i].ready){SInt32 attributes=kMenuItemAttrDisabled;err=AEPutKeyPtr(&child,keyContextualMenuAttributes,typeSInt32,&attributes,sizeof(attributes));}
  if(!err)err=AEPutDesc(&submenu,0,&child);AEDisposeDesc(&child);
 }
 if(!err)err=AECreateList(NULL,0,true,&parent);
 if(!err)err=item_name(&parent,CFSTR("Open on"));
 if(!err)err=AEPutKeyDesc(&parent,keyContextualMenuSubmenu,&submenu);
 if(!err)err=AEPutDesc(commands,0,&parent);
 AEDisposeDesc(&parent);AEDisposeDesc(&submenu);return err;
}
static OSStatus selected(void *object,AEDesc *context,SInt32 command)
{
 struct menu_instance *m=object;unsigned i;
 for(i=0;i<m->count;i++)if(command==100+m->gpu[i].index && m->gpu[i].ready)break;
 if(!m->application || i==m->count)return paramErr;
 FSRef helper;OSStatus err=FSPathMakeRef((const UInt8 *)"/Library/Application Support/RadeonNI/OpenOn.app",&helper,NULL);
 if(err)return err;
 CFStringRef path=CFURLCopyFileSystemPath(m->application,kCFURLPOSIXPathStyle);
 CFStringRef index=CFStringCreateWithFormat(NULL,NULL,CFSTR("%d"),m->gpu[i].index);
 const void *values[]={path,index};CFArrayRef args=CFArrayCreate(NULL,values,2,&kCFTypeArrayCallBacks);
 LSApplicationParameters params={0};params.application=&helper;params.argv=args;
 params.flags=kLSLaunchAsync|kLSLaunchNewInstance|kLSLaunchDontAddToRecents;
 CFDictionaryRef environment=NULL;CFStringRef trace=NULL;const char *debug=getenv("RDN_OPEN_ON_TRACE");
 if(debug){trace=CFStringCreateWithCString(NULL,debug,kCFStringEncodingUTF8);const void *key=CFSTR("RDN_OPEN_ON_TRACE");const void *value=trace;environment=CFDictionaryCreate(NULL,&key,&value,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);params.environment=environment;}
 err=LSOpenApplication(&params,NULL);if(environment)CFRelease(environment);if(trace)CFRelease(trace);CFRelease(args);CFRelease(index);CFRelease(path);return err;
}
static ContextualMenuInterfaceStruct interface={NULL,query,retain_instance,release_instance,examine,selected,cleanup};
__attribute__((visibility("default"))) void *RadeonNIOpenOnFactory(CFAllocatorRef allocator,CFUUIDRef type)
{
 if(!CFEqual(type,kContextualMenuTypeID))return NULL;
 struct menu_instance *m=calloc(1,sizeof(*m));if(!m)return NULL;
 m->interface=&interface;m->factory=CFUUIDCreateFromString(NULL,FACTORY);m->refs=1;
 CFPlugInAddInstanceForFactory(m->factory);return m;
}
