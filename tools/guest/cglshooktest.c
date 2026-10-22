/* Exercise the Leopard private-context hook without a WindowServer session. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
#include <OpenGL/OpenGL.h>

static long engine(void *rend, long pname, long *values)
{
    return rend == (void *)0x1234 && pname == 17 && values[0] == 23 ? 41 : -1;
}

static int handler(void *ctx, long pname, long *values, long *result)
{
    if (pname != 29) return 0;
    *result = values[0] + 1;
    return 1;
}

static int texture_handler(void *ctx, void *pbuffer, long source, long *result)
{
    return 0;
}

int main(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFARendererID, 0x21a00, 0 };
    CGLPixelFormatObj pf;
    CGLContextObj glctx;
    GLint count;
    unsigned i;
    struct nlist symbol[3];
    int (*install)(int (*)(void *, long, long *, long *)) = NULL;
    void (*install_texture)(int (*)(void *, void *, long, long *)) = NULL;
    long (*set)(void *, long, long *);
    char *ctx;
    void *table[12] = { 0 };
    long value = 23, off;
    int failed = 0;
    void *cg = dlopen("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    if (!cg || CGLChoosePixelFormat(attrs, &pf, &count) ||
        CGLCreateContext(pf, NULL, &glctx)) return 2;
    set = dlsym(cg, "cglsSetInteger");
    if (!set) return 3;
    off = *(int *)((char *)set + 12 +
        (int32_t)(((uint32_t *)set)[9] << 16) + (int16_t)((uint32_t *)set)[10]);
    if (off < 0 || off > 16384) return 4;
    for (i = 0; i < _dyld_image_count(); ++i) {
        const char *path = _dyld_get_image_name(i);
        if (!strstr(path, "RadeonNIGLDriver")) continue;
        memset(symbol, 0, sizeof(symbol));
        symbol[0].n_un.n_name = "_rdn_hook_cgls_set_integer";
        symbol[1].n_un.n_name = "_rdn_hook_cgls_tex_image";
        if (nlist(path, symbol)) return 5;
        install = (void *)(symbol[0].n_value + _dyld_get_image_vmaddr_slide(i));
        install_texture = (void *)(symbol[1].n_value + _dyld_get_image_vmaddr_slide(i));
    }
    if (!install || !install(handler)) return 6;
    ctx = calloc(1, off + 32);
    *(void **)ctx = (void *)0x1234;
    table[11] = engine;
    *(void ***)(ctx + off + 4) = table;
    failed += set(ctx, 17, &value) != 41;
    failed += set(ctx, 29, &value) != 24;
    failed += set(NULL, 17, &value) != 0x2714;
    {
        long (*create)(void **, long, long, long, long, long) = dlsym(cg, "cglsCreatePBuffer");
        long (*destroy)(void *) = dlsym(cg, "cglsDestroyPBuffer");
        void *pbuffer = NULL;
        if (!install_texture || !create || !destroy) return 7;
        install_texture(texture_handler);
        failed += create(&pbuffer, 32, 24, 0x84f5, 0x1908, 0) != 0;
        if (pbuffer) {
            failed += ((unsigned *)pbuffer)[8] != 32 || ((unsigned *)pbuffer)[9] != 24;
            failed += destroy(pbuffer) != 0;
        } else ++failed;
    }
    printf("cglsSetInteger: %d failed checks\n", failed);
    free(ctx);
    CGLDestroyContext(glctx);
    CGLDestroyPixelFormat(pf);
    return failed ? 1 : 0;
}
