/* Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#include "rdn_nv12_internal.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "tgsi/tgsi_ureg.h"
#include "util/u_atomic.h"
#include "util/u_draw.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_sampler.h"

struct rdn_nv12 {
   struct pipe_context *pipe;
   void *vs, *fs[2], *blend, *dsa, *rasterizer, *elements, *sampler;
   struct pipe_resource *source[4], *destination[2];
   struct pipe_sampler_view *views[4];
   struct pipe_fence_handle *fence;
   bool pending;
};

static int quarantine_count;

bool
rdn_nv12_source_valid(const struct OSMesaNV12Source *s)
{
   uint64_t begin[4], end[4];
   uint32_t offsets[4], rows[4];

   if (!s || !s->handle || !s->bytes || s->width < 2 || s->height < 4 ||
       s->width > 8192 || s->height > 8192 || (s->width & 1) || (s->height & 1) ||
       s->pitch < s->width || (s->pitch & 7) ||
       (uint64_t)s->offset + s->bytes > (uint64_t)UINT32_MAX + 1)
      return false;
   offsets[0] = s->y_top; offsets[1] = s->y_bottom;
   offsets[2] = s->uv_top; offsets[3] = s->uv_bottom;
   rows[0] = rows[1] = s->height / 2;
   rows[2] = (s->height / 2 + 1) / 2;
   rows[3] = s->height / 4;
   for (unsigned i = 0; i < 4; i++) {
      begin[i] = offsets[i];
      end[i] = begin[i] + (uint64_t)(rows[i] - 1) * s->pitch + s->width;
      if (((uint64_t)s->offset + begin[i]) & 255 || end[i] > s->bytes)
         return false;
      for (unsigned j = 0; j < i; j++)
         if (begin[i] < end[j] && begin[j] < end[i])
            return false;
   }
   return true;
}

static void *
create_vs(struct pipe_context *pipe)
{
   struct ureg_program *u = ureg_create(MESA_SHADER_VERTEX);
   struct ureg_src id;
   struct ureg_dst p, out;

   if (!u)
      return NULL;
   id = ureg_scalar(ureg_DECL_system_value(u, TGSI_SEMANTIC_VERTEXID, 0), TGSI_SWIZZLE_X);
   p = ureg_DECL_temporary(u);
   out = ureg_DECL_output(u, TGSI_SEMANTIC_POSITION, 0);
   ureg_MOV(u, p, ureg_imm4f(u, 0, 0, 0, 1));
   ureg_SHL(u, ureg_writemask(p, TGSI_WRITEMASK_X), id, ureg_imm1u(u, 1));
   ureg_AND(u, ureg_writemask(p, TGSI_WRITEMASK_X), ureg_src(p), ureg_imm1u(u, 2));
   ureg_AND(u, ureg_writemask(p, TGSI_WRITEMASK_Y), id, ureg_imm1u(u, 2));
   ureg_U2F(u, ureg_writemask(p, TGSI_WRITEMASK_XY), ureg_src(p));
   ureg_MAD(u, ureg_writemask(p, TGSI_WRITEMASK_XY), ureg_src(p),
            ureg_imm1f(u, 2), ureg_imm1f(u, -1));
   ureg_MOV(u, out, ureg_src(p));
   ureg_END(u);
   return ureg_create_shader_and_destroy(u, pipe);
}

static void
fetch_field(struct ureg_program *u, struct ureg_dst out, struct ureg_dst coord,
            struct ureg_src sampler, bool uv)
{
   struct ureg_dst value = ureg_DECL_temporary(u);

   ureg_TXF(u, value, TGSI_TEXTURE_2D, ureg_src(coord), sampler);
   ureg_MOV(u, out, ureg_src(value));
   if (uv) {
      ureg_UADD(u, ureg_writemask(coord, TGSI_WRITEMASK_X), ureg_src(coord), ureg_imm1u(u, 1));
      ureg_TXF(u, value, TGSI_TEXTURE_2D, ureg_src(coord), sampler);
      ureg_MOV(u, ureg_writemask(out, TGSI_WRITEMASK_Y), ureg_scalar(ureg_src(value), TGSI_SWIZZLE_X));
   }
}

static void *
create_fs(struct pipe_context *pipe, bool uv)
{
   struct ureg_program *u = ureg_create(MESA_SHADER_FRAGMENT);
   struct ureg_src position, sampler[2];
   struct ureg_dst coord, parity, out;
   unsigned label;

   if (!u)
      return NULL;
   ureg_property(u, TGSI_PROPERTY_FS_COORD_ORIGIN, TGSI_FS_COORD_ORIGIN_UPPER_LEFT);
   ureg_property(u, TGSI_PROPERTY_FS_COORD_PIXEL_CENTER, TGSI_FS_COORD_PIXEL_CENTER_HALF_INTEGER);
   position = ureg_DECL_fs_input(u, TGSI_SEMANTIC_POSITION, 0, TGSI_INTERPOLATE_LINEAR);
   for (unsigned i = 0; i < 2; i++) {
      sampler[i] = ureg_DECL_sampler(u, i);
      ureg_DECL_sampler_view(u, i, TGSI_TEXTURE_2D, TGSI_RETURN_TYPE_FLOAT,
                             TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   }
   coord = ureg_DECL_temporary(u);
   parity = ureg_DECL_temporary(u);
   out = ureg_DECL_output(u, TGSI_SEMANTIC_COLOR, 0);
   ureg_MOV(u, coord, ureg_imm1u(u, 0));
   ureg_F2U(u, ureg_writemask(coord, TGSI_WRITEMASK_XY), position);
   ureg_AND(u, parity, ureg_scalar(ureg_src(coord), TGSI_SWIZZLE_Y), ureg_imm1u(u, 1));
   ureg_USHR(u, ureg_writemask(coord, TGSI_WRITEMASK_Y), ureg_src(coord), ureg_imm1u(u, 1));
   if (uv)
      ureg_SHL(u, ureg_writemask(coord, TGSI_WRITEMASK_X), ureg_src(coord), ureg_imm1u(u, 1));
   ureg_UIF(u, ureg_src(parity), &label);
   fetch_field(u, out, coord, sampler[1], uv);
   ureg_fixup_label(u, label, ureg_get_instruction_number(u));
   ureg_ELSE(u, &label);
   fetch_field(u, out, coord, sampler[0], uv);
   ureg_fixup_label(u, label, ureg_get_instruction_number(u));
   ureg_ENDIF(u);
   ureg_END(u);
   return ureg_create_shader_and_destroy(u, pipe);
}

bool
rdn_nv12_has_quarantine(void)
{
   return p_atomic_read(&quarantine_count) != 0;
}

bool
rdn_nv12_available(const struct rdn_nv12 *p)
{
   return p && !p->pending && !rdn_nv12_has_quarantine() &&
          (!p->pipe->get_device_reset_status ||
           p->pipe->get_device_reset_status(p->pipe) == PIPE_NO_RESET);
}

struct rdn_nv12 *
rdn_nv12_create(struct pipe_screen *screen)
{
   struct rdn_nv12 *p;
   struct pipe_blend_state blend = {0};
   struct pipe_depth_stencil_alpha_state dsa = {0};
   struct pipe_rasterizer_state rasterizer = {0};
   struct pipe_sampler_state sampler = {0};

   if (rdn_nv12_has_quarantine() ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R8_UNORM, PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET) ||
       !screen->is_format_supported(screen, PIPE_FORMAT_R8G8_UNORM, PIPE_TEXTURE_2D, 0, 0,
                                    PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW))
      return NULL;
   p = CALLOC_STRUCT(rdn_nv12);
   if (!p)
      return NULL;
   p->pipe = screen->context_create(screen, NULL, 0);
   if (!p->pipe) {
      FREE(p);
      return NULL;
   }
   blend.rt[0].colormask = PIPE_MASK_RGBA;
   rasterizer.half_pixel_center = 1;
   rasterizer.bottom_edge_rule = 1;
   rasterizer.depth_clip_near = rasterizer.depth_clip_far = 1;
   sampler.wrap_s = sampler.wrap_t = sampler.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   p->vs = create_vs(p->pipe);
   p->fs[0] = create_fs(p->pipe, false);
   p->fs[1] = create_fs(p->pipe, true);
   p->blend = p->pipe->create_blend_state(p->pipe, &blend);
   p->dsa = p->pipe->create_depth_stencil_alpha_state(p->pipe, &dsa);
   p->rasterizer = p->pipe->create_rasterizer_state(p->pipe, &rasterizer);
   p->elements = p->pipe->create_vertex_elements_state(p->pipe, 0, NULL);
   p->sampler = p->pipe->create_sampler_state(p->pipe, &sampler);
   if (!p->vs || !p->fs[0] || !p->fs[1] || !p->blend || !p->dsa ||
       !p->rasterizer || !p->elements || !p->sampler) {
      rdn_nv12_destroy(p);
      return NULL;
   }
   return p;
}

static void
release_resources(struct rdn_nv12 *p)
{
   struct pipe_framebuffer_state empty = {0};
   p->pipe->set_sampler_views(p->pipe, MESA_SHADER_FRAGMENT, 0, 0, 2, NULL);
   p->pipe->set_framebuffer_state(p->pipe, &empty);
   for (unsigned i = 0; i < 4; i++) {
      pipe_sampler_view_reference(&p->views[i], NULL);
      pipe_resource_reference(&p->source[i], NULL);
   }
   for (unsigned i = 0; i < 2; i++)
      pipe_resource_reference(&p->destination[i], NULL);
   p->pipe->screen->fence_reference(p->pipe->screen, &p->fence, NULL);
}

int
rdn_nv12_run(struct rdn_nv12 *p, struct pipe_resource *source[4],
              struct pipe_resource *destination[2], uint64_t timeout_ns)
{
   struct pipe_context *pipe;
   void *samplers[2];

   if (!rdn_nv12_available(p))
      return OSMESA_NV12_UNSUPPORTED;
   pipe = p->pipe;
   for (unsigned i = 0; i < 4; i++) {
      struct pipe_sampler_view view;
      pipe_resource_reference(&p->source[i], source[i]);
      u_sampler_view_default_template(&view, source[i], PIPE_FORMAT_R8_UNORM);
      p->views[i] = pipe->create_sampler_view(pipe, source[i], &view);
      if (!p->views[i]) {
         release_resources(p);
         return OSMESA_NV12_RETIRED;
      }
   }
   for (unsigned i = 0; i < 2; i++)
      pipe_resource_reference(&p->destination[i], destination[i]);
   pipe->bind_vs_state(pipe, p->vs);
   pipe->bind_blend_state(pipe, p->blend);
   pipe->bind_depth_stencil_alpha_state(pipe, p->dsa);
   pipe->bind_rasterizer_state(pipe, p->rasterizer);
   pipe->bind_vertex_elements_state(pipe, p->elements);
   pipe->set_vertex_buffers(pipe, 0, NULL);
   pipe->set_sample_mask(pipe, ~0u);
   samplers[0] = samplers[1] = p->sampler;
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 2, samplers);
   for (unsigned plane = 0; plane < 2; plane++) {
      struct pipe_framebuffer_state fb = {0};
      struct pipe_viewport_state viewport = {0};
      fb.width = destination[plane]->width0;
      fb.height = destination[plane]->height0;
      fb.nr_cbufs = 1;
      fb.cbufs[0].texture = destination[plane];
      fb.cbufs[0].format = destination[plane]->format;
      viewport.scale[0] = viewport.translate[0] = fb.width * 0.5f;
      viewport.scale[1] = viewport.translate[1] = fb.height * 0.5f;
      viewport.scale[2] = 1;
      pipe->set_framebuffer_state(pipe, &fb);
      pipe->set_viewport_states(pipe, 0, 1, &viewport);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 2, 0, &p->views[plane * 2]);
      pipe->bind_fs_state(pipe, p->fs[plane]);
      util_draw_arrays(pipe, MESA_PRIM_TRIANGLES, 0, 3);
   }
   if (pipe->memory_barrier)
      pipe->memory_barrier(pipe, PIPE_BARRIER_FRAMEBUFFER | PIPE_BARRIER_TEXTURE);
   pipe->flush(pipe, &p->fence, 0);
   if (!p->fence || !pipe->screen->fence_finish(pipe->screen, pipe, p->fence, timeout_ns)) {
      p->pending = true;
      p_atomic_inc(&quarantine_count);
      return OSMESA_NV12_PENDING;
   }
   bool healthy = !pipe->get_device_reset_status || pipe->get_device_reset_status(pipe) == PIPE_NO_RESET;
   release_resources(p);
   return healthy ? OSMESA_NV12_OK : OSMESA_NV12_RETIRED;
}

void
rdn_nv12_destroy(struct rdn_nv12 *p)
{
   if (!p || p->pending)
      return;
   release_resources(p);
   p->pipe->bind_vs_state(p->pipe, NULL);
   p->pipe->bind_fs_state(p->pipe, NULL);
   p->pipe->bind_blend_state(p->pipe, NULL);
   p->pipe->bind_depth_stencil_alpha_state(p->pipe, NULL);
   p->pipe->bind_rasterizer_state(p->pipe, NULL);
   p->pipe->bind_vertex_elements_state(p->pipe, NULL);
   p->pipe->bind_sampler_states(p->pipe, MESA_SHADER_FRAGMENT, 0, 2, (void *[2]){NULL, NULL});
   if (p->vs) p->pipe->delete_vs_state(p->pipe, p->vs);
   for (unsigned i = 0; i < 2; i++)
      if (p->fs[i]) p->pipe->delete_fs_state(p->pipe, p->fs[i]);
   if (p->blend) p->pipe->delete_blend_state(p->pipe, p->blend);
   if (p->dsa) p->pipe->delete_depth_stencil_alpha_state(p->pipe, p->dsa);
   if (p->rasterizer) p->pipe->delete_rasterizer_state(p->pipe, p->rasterizer);
   if (p->elements) p->pipe->delete_vertex_elements_state(p->pipe, p->elements);
   if (p->sampler) p->pipe->delete_sampler_state(p->pipe, p->sampler);
   p->pipe->destroy(p->pipe);
   FREE(p);
}
