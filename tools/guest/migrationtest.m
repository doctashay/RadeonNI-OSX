/* Verify texture contents survive an accelerated window crossing GPUs. */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    if (argc > 1) freopen(argv[1], "w", stdout);
    setbuf(stdout, NULL);
    NSArray *screens = [NSScreen screens];
    if ([screens count] < 2) return 2;
    CGDirectDisplayID display = [[[[screens objectAtIndex:0] deviceDescription]
        objectForKey:@"NSScreenNumber"] unsignedIntValue];
    NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFAAccelerated,
        NSOpenGLPFANoRecovery, NSOpenGLPFADoubleBuffer, 0, 0, 0};
    int state = argc > 2 && !strcmp(argv[2], "--state");
    int switching = state || (argc > 2 && !strcmp(argv[2], "--switch"));
    if (switching) {
        attrs[3] = NSOpenGLPFAScreenMask;
        attrs[4] = 0xffffffff;
    } else if (argc > 2) {
        attrs[3] = NSOpenGLPFAScreenMask;
        attrs[4] = CGDisplayIDToOpenGLDisplayMask(display);
    }
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!pf) return 2;
    printf("virtual_screens=%d\n", [pf numberOfVirtualScreens]);
    NSOpenGLView *view = [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0,0,320,240) pixelFormat:pf];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100,100,320,240)
        styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
    [window setContentView:view];
    [window setTitle:@"RadeonNI GPU migration test"];
    [window orderFront:nil];
    NSOpenGLContext *ctx = [view openGLContext];
    GLuint texture = 0, buffer = 0, list = 0, glyph = 0;
    unsigned char texels[16] = {32,192,64,255,32,192,64,255,32,192,64,255,32,192,64,255};
    int failed = 0, pass;
    GLint first_renderer = 0;
    const char *cached_renderer = NULL;
    for (pass = 0; pass < 4; ++pass) {
        NSRect frame = [[screens objectAtIndex:pass % 2] visibleFrame];
        [window setFrameOrigin:NSMakePoint(frame.origin.x+100,frame.origin.y+100)];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
        [ctx update]; [ctx makeCurrentContext];
        GLint renderer = 0;
        CGLGetParameter([ctx CGLContextObj], kCGLCPCurrentRendererID, &renderer);
        if (!pass) { first_renderer = renderer; cached_renderer = (const char *)glGetString(GL_RENDERER); }
        const char *current_renderer = (const char *)glGetString(GL_RENDERER);
        printf("renderer_name=%s cached_name=%s\n",current_renderer,cached_renderer);
        if (strcmp(current_renderer,cached_renderer)) failed = 1;
        if (switching && ((pass % 2) ? renderer == first_renderer : renderer != first_renderer)) {
            puts("FAIL: renderer did not follow the destination display");
            failed = 1;
        }
        if (!pass) {
            if (state) {
                const GLfloat vertices[] = {-1,-1,1,-1,1,1,-1,1};
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                list = glGenLists(1);
                glNewList(list, GL_COMPILE); glColor4f(1,1,1,1); glEndList();
                GLubyte bits[36] = {0};
                for (int y = 0; y < 8; ++y) bits[(y+1)*4] = 0x78;
                glyph = glGenLists(1);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 16);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
                glPixelStorei(GL_UNPACK_LSB_FIRST, 1);
                glNewList(glyph, GL_COMPILE); glBitmap(8,8,0,0,0,0,bits); glEndList();
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_LSB_FIRST, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            }
            glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D,0,switching ? 3 : GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,texels);
        }
        if (state) {
            GLboolean buffer_exists = glIsBuffer(buffer), list_exists = glIsList(list);
            printf("retained buffer=%u list=%u\n", buffer_exists, list_exists);
            if (!buffer_exists || !list_exists) failed = 1;
            if (buffer_exists) {
                GLfloat vertices[8] = {0};
                const GLfloat expected[8] = {-1,-1,1,-1,1,1,-1,1};
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof(vertices),vertices);
                glBindBuffer(GL_ARRAY_BUFFER,0);
                int data_ok = !memcmp(vertices,expected,sizeof(vertices)) && !glGetError();
                printf("buffer_contents=%d\n",data_ok);
                failed |= !data_ok;
            }
        }
        GLboolean exists = glIsTexture(texture);
        if (!state || !pass) {
        glBindTexture(GL_TEXTURE_2D,texture); glEnable(GL_TEXTURE_2D);
        glViewport(0,0,320,240);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glColor4f(1,1,1,1); glClearColor(1,0,1,1);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord2f(0,0); glVertex2f(-1,-1);
        glTexCoord2f(1,0); glVertex2f(1,-1);
        glTexCoord2f(1,1); glVertex2f(1,1);
        glTexCoord2f(0,1); glVertex2f(-1,1); glEnd();
        unsigned char pixel[4] = {0};
        glReadBuffer(GL_BACK); glReadPixels(160,120,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        GLenum error = glGetError();
        int red = switching && !state ? 32 + pass * 16 : 32;
        int green = switching && !state ? 192 - pass * 16 : 192;
        int ok = exists && !error && pixel[0]==red && pixel[1]==green && pixel[2]==64;
        failed |= !ok;
        if (state) {
            glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
            glDisable(GL_TEXTURE_2D); glColor4f(1,1,1,1);
            glRasterPos2f(-0.5f,-0.5f); glCallList(glyph);
            unsigned char ink[4] = {0};
            glReadPixels(83,63,1,1,GL_RGBA,GL_UNSIGNED_BYTE,ink);
            int glyph_ok = ink[0]==255 && ink[1]==255 && ink[2]==255 && !glGetError();
            printf("bitmap_list=%u ink=%u,%u,%u correct=%d\n",glyph,ink[0],ink[1],ink[2],glyph_ok);
            failed |= !glyph_ok;
            glPopAttrib();
        }
        printf("pass=%d renderer=%x texture=%u exists=%u pixel=%u,%u,%u error=%x correct=%d\n",
            pass,renderer,texture,exists,pixel[0],pixel[1],pixel[2],error,ok);
        if (switching && !state) {
            for (int i = 0; i < 4; ++i) {
                texels[i * 4] = 32 + (pass + 1) * 16;
                texels[i * 4 + 1] = 192 - (pass + 1) * 16;
            }
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,texels);
        }
        [ctx flushBuffer];
    }
    if (switching && !state) {
        glDeleteTextures(1, &texture);
        NSRect frame = [[screens objectAtIndex:0] visibleFrame];
        [window setFrameOrigin:NSMakePoint(frame.origin.x+100,frame.origin.y+100)];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
        [ctx update]; [ctx makeCurrentContext];
        int deleted = !glIsTexture(texture) && !glGetError();
        printf("deleted_on_other_gpu=%d\n", deleted);
        failed |= !deleted;
    }
    if (state) {
        glDeleteLists(list,1); glDeleteLists(glyph,1);
        NSRect frame = [[screens objectAtIndex:0] visibleFrame];
        [window setFrameOrigin:NSMakePoint(frame.origin.x+100,frame.origin.y+100)];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
        [ctx update]; [ctx makeCurrentContext];
        int gone = !glIsList(list) && !glIsList(glyph) && !glGetError();
        printf("lists_deleted_on_other_gpu=%d\n",gone);
        failed |= !gone;
    }
    printf("%s\n",failed ? "FAIL" : "PASS");
    [ctx clearDrawable]; [window close]; [pool release];
    return failed;
}
