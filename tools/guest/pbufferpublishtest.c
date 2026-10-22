/* A texture import submits its pbuffer producer before the consumer samples it. */
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
    CGLPixelFormatAttribute attrs[]={kCGLPFARendererID,0x21a00,kCGLPFAPBuffer,0};
    CGLPixelFormatObj pf;CGLContextObj producer,consumer;CGLPBufferObj pb;
    GLint n;GLuint texture;unsigned char pixels[32*24*4];int failures=0;
    if(CGLChoosePixelFormat(attrs,&pf,&n)||CGLCreateContext(pf,0,&producer)||
       CGLCreateContext(pf,0,&consumer)||
       CGLCreatePBuffer(32,24,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,0,&pb))return 2;
    CGLSetCurrentContext(producer);if(CGLSetPBuffer(producer,pb,0,0,0))return 3;
    int cid,wid,sid;CGLGetSurface(producer,&cid,&wid,&sid);
    glGetString(GL_RENDERER);
    void *(*get_context)(void)=driver_symbol("_OSMesaGetCurrentContext");
    int (*adopt)(void *)=driver_symbol("_OSMesaAdoptContext");
    void *(*get_proc)(const char *)=driver_symbol("_OSMesaGetProcAddress");
    int (*import)(void *,int,int *,int *)=driver_symbol("_rdn_mesa_cgls_set_integer");
    if(!get_context || !adopt || !get_proc || !import)return 5;
    void *producer_mesa=get_context();
    CGLSetCurrentContext(consumer);glGenTextures(1,&texture);
    void *consumer_mesa=get_context();
    void (*clear_color)(float,float,float,float)=get_proc("glClearColor");
    void (*clear)(unsigned)=get_proc("glClear");
    void (*bind)(unsigned,unsigned)=get_proc("glBindTexture");
    void (*read_image)(unsigned,int,unsigned,unsigned,void *)=get_proc("glGetTexImage");
    for(int frame=0;frame<32;++frame) {
        adopt(producer_mesa);
        clear_color(frame&1,!(frame&1),0,0.5);clear(GL_COLOR_BUFFER_BIT);
        /* Private compositor context switches do not flush the producer. */
        adopt(consumer_mesa);bind(GL_TEXTURE_RECTANGLE_ARB,texture);
        GLint values[]={sid,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,32,24,GL_UNSIGNED_INT_8_8_8_8_REV};
        int result=-1;
        if(!import(consumer,999,values,&result)||result)return 4;
        read_image(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        for(int i=0;i<32*24;++i)
            failures += pixels[i*4] != ((frame&1)?255:0) ||
                        pixels[i*4+1] != ((frame&1)?0:255) ||
                        pixels[i*4+2] != 0 || pixels[i*4+3] != 128;
        if(glGetError())++failures;
    }
    void (*detach)(void *)=driver_symbol("_rdn_mesa_detach");
    void **contexts=driver_symbol("_contexts");
    if (!detach || !contexts || !*contexts) return 6;
    adopt(producer_mesa);clear_color(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);
    detach(*contexts);
    adopt(consumer_mesa);bind(GL_TEXTURE_RECTANGLE_ARB,texture);
    GLint detached_values[]={sid,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,32,24};
    int detached_result=-1;
    if(!import(consumer,999,detached_values,&detached_result)||detached_result)return 7;
    read_image(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    unsigned detached_errors=0;
    for(int i=0;i<32*24;++i)
        detached_errors+=pixels[i*4]!=0||pixels[i*4+1]!=0||pixels[i*4+2]!=255||pixels[i*4+3]!=255;
    printf("detached producer import: %u mismatched pixels\n",detached_errors);
    failures+=detached_errors;
    printf("32 imports without explicit producer flush: %d mismatched pixels/errors\n",failures);
    glDeleteTextures(1,&texture);CGLSetCurrentContext(0);
    CGLDestroyContext(consumer);CGLDestroyContext(producer);CGLDestroyPBuffer(pb);CGLDestroyPixelFormat(pf);
    return failures!=0;
}
