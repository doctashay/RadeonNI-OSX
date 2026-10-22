/* Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rdn_nv12_internal.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "softpipe/sp_public.h"
#include "null/null_sw_winsys.h"
#include "util/u_inlines.h"
#include "util/box.h"

static unsigned char value(unsigned plane, unsigned x, unsigned y)
{
   return (plane * 71 + y * 17 + x * 29 + (x >> 3) * 11) & 255;
}

static struct pipe_resource *
texture(struct pipe_screen *screen, unsigned w, unsigned h, enum pipe_format format)
{
   struct pipe_resource t = {0};
   t.target = PIPE_TEXTURE_2D; t.format = format;
   t.width0 = w; t.height0 = h; t.depth0 = t.array_size = 1;
   t.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   return screen->resource_create(screen, &t);
}

static bool never_complete(struct pipe_screen *screen, struct pipe_context *pipe,
                           struct pipe_fence_handle *fence, uint64_t timeout)
{
   return false;
}

static void quarantine_case(void)
{
   struct pipe_screen *screen=softpipe_create_screen(null_sw_create());
   assert(screen);
   struct rdn_nv12 *pack=rdn_nv12_create(screen);
   struct pipe_resource *src[4],*dst[2];
   for(unsigned i=0;i<4;i++)src[i]=texture(screen,64,i<2?32:16,PIPE_FORMAT_R8_UNORM);
   dst[0]=texture(screen,64,64,PIPE_FORMAT_R8_UNORM);
   dst[1]=texture(screen,32,32,PIPE_FORMAT_R8G8_UNORM);
   assert(pack && src[0] && src[1] && src[2] && src[3] && dst[0] && dst[1]);
   screen->fence_finish=never_complete;
   assert(rdn_nv12_run(pack,src,dst,1)==OSMESA_NV12_PENDING);
   assert(!rdn_nv12_available(pack) && rdn_nv12_has_quarantine());
   int counts[6];
   for(unsigned i=0;i<4;i++){counts[i]=p_atomic_read(&src[i]->reference.count);assert(counts[i]>1);}
   for(unsigned i=0;i<2;i++){counts[i+4]=p_atomic_read(&dst[i]->reference.count);assert(counts[i+4]>1);}
   rdn_nv12_destroy(pack);
   for(unsigned i=0;i<4;i++)assert(p_atomic_read(&src[i]->reference.count)==counts[i]);
   for(unsigned i=0;i<2;i++)assert(p_atomic_read(&dst[i]->reference.count)==counts[i+4]);
   assert(!rdn_nv12_create(screen));
   /* Intentional process-lifetime quarantine: no screen/context/resource teardown. */
   _exit(0);
}

int main(void)
{
   pid_t child=fork();assert(child>=0);
   if(!child)quarantine_case();
   int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
   const unsigned sizes[][2] = {{64,64},{130,70},{1280,720},{1920,1088}};
   struct pipe_screen *screen = softpipe_create_screen(null_sw_create());
   assert(screen);
   struct pipe_context *upload = screen->context_create(screen,NULL,0);
   struct rdn_nv12 *pack = rdn_nv12_create(screen);
   assert(upload && pack && rdn_nv12_available(pack));
   unsigned long checked = 0;
   for (unsigned test=0; test<sizeof(sizes)/sizeof(sizes[0]); test++) {
      unsigned w=sizes[test][0],h=sizes[test][1];
      struct pipe_resource *src[4]={0}, *dst[2]={0};
      for (unsigned i=0; i<4; i++) {
         unsigned plane=i/2,field=i%2, rows=((h/(plane+1))+1-field)/2;
         unsigned stride=w+24;
         unsigned char *bytes=malloc((size_t)stride*rows);
         assert(bytes);memset(bytes,0xa5,(size_t)stride*rows);
         for(unsigned y=0;y<rows;y++)
            for(unsigned x=0;x<w;x++)bytes[y*stride+x]=value(plane,x,2*y+field);
         src[i]=texture(screen,w,rows,PIPE_FORMAT_R8_UNORM);assert(src[i]);
         struct pipe_box box;u_box_2d(0,0,w,rows,&box);
         upload->texture_subdata(upload,src[i],0,0,&box,bytes,stride,stride*rows);
         free(bytes);
      }
      dst[0]=texture(screen,w,h,PIPE_FORMAT_R8_UNORM);
      dst[1]=texture(screen,w/2,h/2,PIPE_FORMAT_R8G8_UNORM);
      assert(dst[0] && dst[1]);
      upload->flush(upload,NULL,0);
      int result=rdn_nv12_run(pack,src,dst,2000000000ull);
      assert(result==OSMESA_NV12_OK && !rdn_nv12_has_quarantine());
      for(unsigned plane=0;plane<2;plane++) {
         struct pipe_transfer *transfer=NULL;
         unsigned char *map=pipe_texture_map(upload,dst[plane],0,0,PIPE_MAP_READ,
                                            0,0,w/(plane+1),h/(plane+1),&transfer);
         assert(map && transfer);
         for(unsigned y=0;y<h/(plane+1);y++) {
            for(unsigned x=0;x<w;x++) {
               if(map[y*transfer->stride+x]!=value(plane,x,y)) {
                  fprintf(stderr,"Mismatch %ux%u plane%u x%u y%u: %u != %u\n",w,h,plane,x,y,
                          map[y*transfer->stride+x],value(plane,x,y));
                  return 1;
               }
               checked++;
            }
         }
         pipe_texture_unmap(upload,transfer);
      }
      for(unsigned i=0;i<4;i++)pipe_resource_reference(&src[i],NULL);
      for(unsigned i=0;i<2;i++)pipe_resource_reference(&dst[i],NULL);
   }
   rdn_nv12_destroy(pack);upload->destroy(upload);screen->destroy(screen);
   printf("PASS actual softpipe shaders: %lu exact Y/UV bytes, four sizes, field parity, UV channel order and pending-reference quarantine\n",checked);
   return 0;
}
