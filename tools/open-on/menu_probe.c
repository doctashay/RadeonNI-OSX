/* Exercise the native CFPlugIn interface before loading it into Finder. */
#include <Carbon/Carbon.h>
#include <stdio.h>
#include <stdlib.h>
static void name(const AEDesc *record)
{
 AEDesc text={typeNull,NULL};char utf8[1024];
 if(!AEGetKeyDesc(record,keyContextualMenuName,typeUnicodeText,&text)) {
  Size n=AEGetDescDataSize(&text);UniChar chars[512];
  if(n<=sizeof(chars) && !AEGetDescData(&text,chars,sizeof(chars))) {
   CFStringRef s=CFStringCreateWithCharacters(NULL,chars,n/2);
   if(CFStringGetCString(s,utf8,sizeof(utf8),kCFStringEncodingUTF8))printf("item=%s\n",utf8);
   CFRelease(s);
  }
  AEDisposeDesc(&text);
 }
}
int main(int argc,char **argv)
{
 if(argc<3)return 2;
 CFURLRef url=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)argv[1],strlen(argv[1]),true);
 CFPlugInRef plugin=CFPlugInCreate(NULL,url);CFRelease(url);if(!plugin)return 3;
 CFArrayRef factories=CFPlugInFindFactoriesForPlugInTypeInPlugIn(kContextualMenuTypeID,plugin);
 if(!factories||CFArrayGetCount(factories)!=1)return 4;
 void *instance=CFPlugInInstanceCreate(NULL,CFArrayGetValueAtIndex(factories,0),kContextualMenuTypeID);
 CFRelease(factories);if(!instance)return 5;
 ContextualMenuInterfaceStruct *api=*(ContextualMenuInterfaceStruct **)instance;
 FSRef app;if(FSPathMakeRef((const UInt8 *)argv[2],&app,NULL))return 6;
 AEDescList selection={typeNull,NULL},commands={typeNull,NULL};AERecord parent={typeNull,NULL};
 AECreateList(NULL,0,false,&selection);AEPutPtr(&selection,0,typeFSRef,&app,sizeof(app));AECreateList(NULL,0,false,&commands);
 OSStatus error=api->ExamineContext(instance,&selection,&commands);long count=0;AECountItems(&commands,&count);
 printf("examine=%d top_level=%ld\n",(int)error,count);if(error||count!=1)return 7;
 AEGetNthDesc(&commands,1,typeAERecord,NULL,&parent);name(&parent);
 AEDescList submenu={typeNull,NULL};AEGetKeyDesc(&parent,keyContextualMenuSubmenu,typeAEList,&submenu);AECountItems(&submenu,&count);
 printf("gpus=%ld\n",count);if(!count)return 8;
 for(long i=1;i<=count;i++){AERecord child={typeNull,NULL};AEGetNthDesc(&submenu,i,typeAERecord,NULL,&child);name(&child);AEDisposeDesc(&child);}
 if(argc>3){error=api->HandleSelection(instance,&selection,100+atoi(argv[3]));printf("select=%d\n",(int)error);}
 api->PostMenuCleanup(instance);api->Release(instance);
 AEDisposeDesc(&submenu);AEDisposeDesc(&parent);AEDisposeDesc(&commands);AEDisposeDesc(&selection);CFRelease(plugin);
 return error!=0;
}
