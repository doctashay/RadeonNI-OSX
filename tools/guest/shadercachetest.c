/* Validate cached shader output and report compile/link/draw time. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d GL=%x\n", __LINE__, glGetError()); return 1; } } while (0)

int main(int argc, char **argv)
{
   unsigned renderer = argc > 1 ? strtoul(argv[1], NULL, 16) : 0x1b00;
   unsigned mask = argc > 2 ? strtoul(argv[2], NULL, 16) : 1;
   CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated,
      kCGLPFARendererID, renderer, kCGLPFADisplayMask, mask, 0};
   CGLPixelFormatObj pf;
   CGLContextObj ctx;
   GLint count, selected, ok;
   CHECK(!CGLChoosePixelFormat(attrs, &pf, &count) && pf);
   CHECK(!CGLCreateContext(pf, NULL, &ctx) && !CGLSetCurrentContext(ctx));
   CHECK(!CGLGetParameter(ctx, kCGLCPCurrentRendererID, &selected));
   CHECK((selected & 0xffff) == renderer);
   printf("renderer=%x name=%s\n", selected, glGetString(GL_RENDERER));
   GLuint fbo, texture;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffersEXT(1, &fbo);
   glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
   glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
   CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
   glViewport(0, 0, 4, 4);
   glDisable(GL_DITHER);
   const char *vertex = "#version 120\nvoid main(){gl_Position=gl_Vertex;}";
   mach_timebase_info_data_t tb;
   mach_timebase_info(&tb);
   uint64_t start = mach_absolute_time();
   for (unsigned i = 0; i < 32; i++) {
      char fragment[256];
      snprintf(fragment, sizeof(fragment), "#version 120\nvoid main(){gl_FragColor=vec4(%u.0/255.0,0.5,0.75,1.0);}", 32+i);
      const char *sources[] = {vertex, fragment};
      GLenum types[] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
      GLuint program = glCreateProgram(), shaders[2];
      for (unsigned j = 0; j < 2; j++) {
         shaders[j] = glCreateShader(types[j]);
         glShaderSource(shaders[j], 1, &sources[j], NULL);
         glCompileShader(shaders[j]);
         glGetShaderiv(shaders[j], GL_COMPILE_STATUS, &ok); CHECK(ok);
         glAttachShader(program, shaders[j]);
      }
      glLinkProgram(program);
      glGetProgramiv(program, GL_LINK_STATUS, &ok); CHECK(ok);
      glUseProgram(program);
      glBegin(GL_QUADS);
      glVertex2f(-1,-1); glVertex2f(1,-1); glVertex2f(1,1); glVertex2f(-1,1);
      glEnd();
      unsigned char pixel[4];
      glReadPixels(1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
      CHECK(abs((int)pixel[0]-(32+(int)i)) <= 1 && abs((int)pixel[1]-128) <= 1 &&
            abs((int)pixel[2]-191) <= 1 && pixel[3] == 255 && !glGetError());
      glUseProgram(0);
      glDeleteProgram(program);
      for (unsigned j = 0; j < 2; j++) glDeleteShader(shaders[j]);
   }
   double ms = (mach_absolute_time()-start) * (double)tb.numer / tb.denom / 1000000.0;
   printf("PASS shaders=32 compile_link_draw_ms=%.3f\n", ms);
   glDeleteFramebuffersEXT(1, &fbo);
   glDeleteTextures(1, &texture);
   CGLSetCurrentContext(NULL);
   CGLDestroyContext(ctx);
   CGLDestroyPixelFormat(pf);
   return 0;
}
