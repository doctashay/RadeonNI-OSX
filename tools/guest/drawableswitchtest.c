/* Preserve a screen drawable while its context renders an offscreen backing. */
#include <stdio.h>
#include <string.h>
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
extern CGLError CGLGetSurface(CGLContextObj, int *, int *, int *);
static void *driver_symbol(const char *name)
{
    unsigned i;
    for (i = 0; i < _dyld_image_count(); ++i) {
        const char *path = _dyld_get_image_name(i);
        if (!strstr(path, "RadeonNIGLDriver")) continue;
        struct nlist symbols[2];
        memset(symbols, 0, sizeof(symbols));
        symbols[0].n_un.n_name = (char *)name;
        if (!nlist(path, symbols) && symbols[0].n_value)
            return (void *)(symbols[0].n_value + _dyld_get_image_vmaddr_slide(i));
    }
    return NULL;
}
int main(void)
{
    CGLPixelFormatAttribute attrs[]={kCGLPFARendererID,0x21a00,0};
    CGLPixelFormatObj pf;CGLContextObj ctx;GLint n;
    if(CGLChoosePixelFormat(attrs,&pf,&n)||CGLCreateContext(pf,0,&ctx))return 2;
    CGLSetCurrentContext(ctx);glGetString(GL_RENDERER);
    void *(*get_context)(void)=driver_symbol("_OSMesaGetCurrentContext");
    void *(*get_proc)(const char *)=driver_symbol("_OSMesaGetProcAddress");
    int (*direct)(void *,unsigned,int,int,int,unsigned)=driver_symbol("_OSMesaMakeCurrentDirect");
    int (*store)(void *,unsigned,int,unsigned,int,int,unsigned)=driver_symbol("_OSMesaMakeCurrentStore");
    int (*alloc)(unsigned,unsigned *)=driver_symbol("_rdn_target_vram_alloc");
    void (*release)(unsigned)=driver_symbol("_rdn_target_vram_free");
    if(!get_context||!get_proc||!direct||!store||!alloc||!release)return 3;
    void *mesa=get_context();unsigned screen,backing;
    CGLContextObj other;
    if(CGLCreateContext(pf,0,&other))return 8;
    CGLSetCurrentContext(other);glGetString(GL_RENDERER);
    void *other_mesa=get_context();
    if(!alloc(65536,&screen)||!alloc(65536,&backing))return 4;
    void (*color)(float,float,float,float)=get_proc("glClearColor");
    void (*clear)(unsigned)=get_proc("glClear");
    void (*finish)(void)=get_proc("glFinish");
    void (*read)(int,int,int,int,unsigned,unsigned,void *)=get_proc("glReadPixels");
    unsigned char pixels[64*32*4];unsigned failures=0;
    if(!direct(mesa,0x7571,64,32,256,screen))return 5;
    color(1,0,0,1);clear(GL_COLOR_BUFFER_BIT);finish();
    for(int frame=0;frame<8;++frame){
        if(!store(mesa,0x7571,256,backing,64,32,0))return 6;
        color(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);finish();
        if(!direct(other_mesa,0x7571,64,32,256,backing))return 9;
        color(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);finish();
        if(!direct(mesa,0x7571,64,32,256,screen))return 7;
        read(0,0,64,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        for(int i=0;i<64*32;++i)
            failures+=pixels[i*4]!=255||pixels[i*4+1]!=0||pixels[i*4+2]!=0;
    }
    printf("screen/backing switches: %u mismatched pixels\n",failures);
    CGLSetCurrentContext(0);CGLDestroyContext(ctx);CGLDestroyContext(other);CGLDestroyPixelFormat(pf);
    release(screen);release(backing);return failures!=0;
}
