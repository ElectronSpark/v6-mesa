/**************************************************************************
 *
 * Copyright 2009, VMware, Inc.
 * All Rights Reserved.
 * Copyright 2010 George Sapountzis <gsapountzis@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "mesa_interface.h"
#include "git_sha1.h"
#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/box.h"
#include "pipe/p_context.h"
#include "pipe-loader/pipe_loader.h"
#include "frontend/drisw_api.h"
#include "state_tracker/st_context.h"

#include "dri_screen.h"
#include "dri_context.h"
#include "dri_drawable.h"
#include "dri_helpers.h"
#include "dri_query_renderer.h"

#include "util/libsync.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_LIBDRM
#include <xf86drm.h>
#endif

bool
xv6_drisw_drawable_backbuffer_info(void *drawable_private, void *info);

bool
xv6_drisw_drawable_backbuffer_info(void *drawable_private, void *info)
{
   struct dri_drawable *drawable = drawable_private;
   const __DRIswrastLoaderExtension *loader;

   if (!drawable || !info || !drawable->screen)
      return false;
   loader = drawable->screen->swrast_loader;
   if (!loader || loader->base.version < 7 || !loader->xv6GetBackbufferInfo)
      return false;
   return loader->xv6GetBackbufferInfo(drawable, info,
                                       drawable->loaderPrivate) != 0;
}

DEBUG_GET_ONCE_BOOL_OPTION(swrast_no_present, "SWRAST_NO_PRESENT", false);

static bool
xv6_d3d12_swrast_skip_prefence(void)
{
   const char *driver = getenv("GALLIUM_DRIVER");
   const char *opt = getenv("XV6_D3D12_SWRAST_NO_PREFENCE");
   const char *async = getenv("XV6_D3D12_ASYNC_FRONTBUFFER");

   if (opt)
      return strcmp(opt, "0") != 0 && strcmp(opt, "false") != 0;
   if (async && (strcmp(async, "0") == 0 || strcmp(async, "false") == 0))
      return false;
   return driver && strcmp(driver, "d3d12") == 0;
}

static bool
xv6_d3d12_swrast_skip_front_flush(void)
{
   const char *driver = getenv("GALLIUM_DRIVER");
   const char *opt = getenv("XV6_D3D12_SWRAST_NO_FRONT_FLUSH");

   if (!driver || strcmp(driver, "d3d12") != 0)
      return false;
   return opt && strcmp(opt, "0") != 0 && strcmp(opt, "false") != 0;
}

static int64_t
xv6_d3d12_now_us(void)
{
   struct timespec ts;

   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static bool
xv6_drisw_perf_log_enabled(void)
{
   const char *perf = getenv("XV6_MESA_PERF_LOG");

   return perf && perf[0] && strcmp(perf, "0") != 0 &&
          strcmp(perf, "false") != 0;
}

static inline void
get_drawable_info(struct dri_drawable *drawable, int *x, int *y, int *w, int *h)
{
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;

   loader->getDrawableInfo(drawable, x, y, w, h,
                           drawable->loaderPrivate);
}

static inline void
put_image(struct dri_drawable *drawable, void *data, unsigned width, unsigned height)
{
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;

   loader->putImage(drawable, __DRI_SWRAST_IMAGE_OP_SWAP,
                    0, 0, width, height,
                    data, drawable->loaderPrivate);
}

static inline void
put_image2(struct dri_drawable *drawable, void *data, int x, int y,
           unsigned width, unsigned height, unsigned stride)
{
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;

   loader->putImage2(drawable, __DRI_SWRAST_IMAGE_OP_SWAP,
                     x, y, width, height, stride,
                     data, drawable->loaderPrivate);
}

static inline void
put_image_shm(struct dri_drawable *drawable, int shmid, char *shmaddr,
              unsigned offset, unsigned offset_x, int x, int y,
              unsigned width, unsigned height, unsigned stride)
{
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;

   /* if we have the newer interface, don't have to add the offset_x here. */
   if (loader->base.version > 4 && loader->putImageShm2)
     loader->putImageShm2(drawable, __DRI_SWRAST_IMAGE_OP_SWAP,
                          x, y, width, height, stride,
                          shmid, shmaddr, offset, drawable->loaderPrivate);
   else
     loader->putImageShm(drawable, __DRI_SWRAST_IMAGE_OP_SWAP,
                         x, y, width, height, stride,
                         shmid, shmaddr, offset + offset_x, drawable->loaderPrivate);
}

static inline void
get_image(struct dri_drawable *drawable, int x, int y, int width, int height, void *data)
{
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;

   loader->getImage(drawable, x, y, width, height,
                    data, drawable->loaderPrivate);
}

static inline void
get_image2(struct dri_drawable *drawable, int x, int y, int width, int height, int stride, void *data)
{
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;

   /* getImage2 support is only in version 3 or newer */
   if (loader->base.version < 3)
      return;

   loader->getImage2(drawable, x, y, width, height, stride,
                     data, drawable->loaderPrivate);
}

static inline bool
get_image_shm(struct dri_drawable *drawable, int x, int y, int width, int height,
              struct pipe_resource *res)
{
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;
   struct winsys_handle whandle;

   whandle.type = WINSYS_HANDLE_TYPE_SHMID;

   if (loader->base.version < 4 || !loader->getImageShm)
      return false;

   if (!res->screen->resource_get_handle(res->screen, NULL, res, &whandle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
      return false;

   if (loader->base.version > 5 && loader->getImageShm2)
      return loader->getImageShm2(drawable, x, y, width, height, whandle.handle, drawable->loaderPrivate);

   loader->getImageShm(drawable, x, y, width, height, whandle.handle, drawable->loaderPrivate);
   return true;
}

static void
drisw_update_drawable_info(struct dri_drawable *drawable)
{
   int x, y;

   get_drawable_info(drawable, &x, &y, &drawable->w, &drawable->h);
}

static void
drisw_get_image(struct dri_drawable *drawable,
                int x, int y, unsigned width, unsigned height, unsigned stride,
                void *data)
{
   int draw_x, draw_y, draw_w, draw_h;

   get_drawable_info(drawable, &draw_x, &draw_y, &draw_w, &draw_h);
   get_image2(drawable, x, y, draw_w, draw_h, stride, data);
}

static void
drisw_put_image(struct dri_drawable *drawable,
                void *data, unsigned width, unsigned height)
{
   put_image(drawable, data, width, height);
}

static void
drisw_put_image2(struct dri_drawable *drawable,
                 void *data, int x, int y, unsigned width, unsigned height,
                 unsigned stride)
{
   put_image2(drawable, data, x, y, width, height, stride);
}

static inline void
drisw_put_image_shm(struct dri_drawable *drawable,
                    int shmid, char *shmaddr, unsigned offset,
                    unsigned offset_x,
                    int x, int y, unsigned width, unsigned height,
                    unsigned stride)
{
   put_image_shm(drawable, shmid, shmaddr, offset, offset_x, x, y, width, height, stride);
}

static inline void
drisw_present_texture(struct pipe_context *pipe, struct dri_drawable *drawable,
                      struct pipe_resource *ptex, unsigned nrects, struct pipe_box *sub_box)
{
   struct dri_screen *screen = drawable->screen;

   if (screen->swrast_no_present)
      return;

   screen->base.screen->flush_frontbuffer(screen->base.screen, pipe, ptex, 0, 0, drawable, nrects, sub_box);
}

static inline void
drisw_invalidate_drawable(struct dri_drawable *drawable)
{
   drawable->texture_stamp = drawable->lastStamp - 1;

   p_atomic_inc(&drawable->base.stamp);
}

static inline void
drisw_copy_to_front(struct pipe_context *pipe,
                    struct dri_drawable *drawable,
                    struct pipe_resource *ptex,
                    int nboxes, struct pipe_box *boxes)
{
   static unsigned xv6_front_detail_frames;
   static int64_t xv6_front_detail_present_us;
   static int64_t xv6_front_detail_invalidate_us;
   static int64_t xv6_front_detail_total_us;
   bool perf = xv6_drisw_perf_log_enabled();
   int64_t t0 = perf ? xv6_d3d12_now_us() : 0;
   int64_t t_present = 0;
   int64_t t_invalidate = 0;

   drisw_present_texture(pipe, drawable, ptex, nboxes, boxes);
   if (perf)
      t_present = xv6_d3d12_now_us();

   drisw_invalidate_drawable(drawable);
   if (perf)
      t_invalidate = xv6_d3d12_now_us();

   if (perf && t0 > 0 && t_present >= t0 && t_invalidate >= t_present) {
      xv6_front_detail_frames++;
      xv6_front_detail_present_us += t_present - t0;
      xv6_front_detail_invalidate_us += t_invalidate - t_present;
      xv6_front_detail_total_us += t_invalidate - t0;
      if (xv6_front_detail_frames >= 60) {
         fprintf(stderr,
                 "xv6-mesa: drisw-front-detail avg_us total=%lld present=%lld invalidate=%lld frames=%u\n",
                 (long long)(xv6_front_detail_total_us /
                             xv6_front_detail_frames),
                 (long long)(xv6_front_detail_present_us /
                             xv6_front_detail_frames),
                 (long long)(xv6_front_detail_invalidate_us /
                             xv6_front_detail_frames),
                 xv6_front_detail_frames);
         fflush(stderr);
         xv6_front_detail_frames = 0;
         xv6_front_detail_present_us = 0;
         xv6_front_detail_invalidate_us = 0;
         xv6_front_detail_total_us = 0;
      }
   }
}

/*
 * Backend functions for pipe_frontend_drawable and swap_buffers.
 */

static void
drisw_swap_buffers_with_damage(struct dri_drawable *drawable, int nrects, const int *rects)
{
   static unsigned xv6_perf_frames;
   static int64_t xv6_perf_thread_us;
   static int64_t xv6_perf_flush_us;
   static int64_t xv6_perf_wait_us;
   static int64_t xv6_perf_front_us;
   static int64_t xv6_perf_state_us;
   static int64_t xv6_perf_total_us;
   bool perf = xv6_drisw_perf_log_enabled();
   int64_t t0 = perf ? xv6_d3d12_now_us() : 0;
   int64_t t_thread = 0;
   int64_t t_flush = 0;
   int64_t t_wait = 0;
   int64_t t_front = 0;
   int64_t t_state = 0;

   /* Damage regions still require us to update the whole front buffer
    * in case the compositor doesn't obey them, so we will just ignore
    * the passed in damage regions and swap the whole buffer
    */
   (void)nrects;
   (void)rects;

   struct dri_context *ctx = dri_get_current();
   struct dri_screen *screen = drawable->screen;
   struct pipe_resource *ptex;

   if (!ctx)
      return;

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
   */
   _mesa_glthread_finish(ctx->st->ctx);
   if (perf)
      t_thread = xv6_d3d12_now_us();

   ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT];

   if (ptex) {
      struct pipe_fence_handle *fence = NULL;

      if (ctx->hud)
         hud_run(ctx->hud, ctx->st->cso_context, ptex);

      if (!xv6_d3d12_swrast_skip_front_flush())
         st_context_flush(ctx->st, ST_FLUSH_FRONT, &fence, NULL, NULL);
      if (perf)
         t_flush = xv6_d3d12_now_us();

      if (drawable->stvis.samples > 1) {
         /* Resolve the back buffer. */
         dri_pipe_blit(ctx->st->pipe,
                       drawable->textures[ST_ATTACHMENT_BACK_LEFT],
                       drawable->msaa_textures[ST_ATTACHMENT_BACK_LEFT]);
      }

      if (!xv6_d3d12_swrast_skip_prefence())
         screen->base.screen->fence_finish(screen->base.screen, ctx->st->pipe,
                                           fence, OS_TIMEOUT_INFINITE);
      if (perf)
         t_wait = xv6_d3d12_now_us();
      if (fence)
         screen->base.screen->fence_reference(screen->base.screen, &fence, NULL);
      drisw_copy_to_front(ctx->st->pipe, drawable, ptex, 0, NULL);
      if (perf)
         t_front = xv6_d3d12_now_us();
      drawable->buffer_age = 1;

      st_context_invalidate_state(ctx->st, ST_INVALIDATE_FB_STATE);
      if (perf)
         t_state = xv6_d3d12_now_us();

      if (perf && t0 > 0 && t_thread >= t0 && t_flush >= t_thread &&
          t_wait >= t_flush && t_front >= t_wait && t_state >= t_front) {
         xv6_perf_frames++;
         xv6_perf_thread_us += t_thread - t0;
         xv6_perf_flush_us += t_flush - t_thread;
         xv6_perf_wait_us += t_wait - t_flush;
         xv6_perf_front_us += t_front - t_wait;
         xv6_perf_state_us += t_state - t_front;
         xv6_perf_total_us += t_state - t0;
         if (xv6_perf_frames >= 60 && xv6_drisw_perf_log_enabled()) {
            fprintf(stderr,
                    "xv6-mesa: drisw-swap avg_us total=%lld thread=%lld flush=%lld wait=%lld front=%lld state=%lld frames=%u\n",
                    (long long)(xv6_perf_total_us / xv6_perf_frames),
                    (long long)(xv6_perf_thread_us / xv6_perf_frames),
                    (long long)(xv6_perf_flush_us / xv6_perf_frames),
                    (long long)(xv6_perf_wait_us / xv6_perf_frames),
                    (long long)(xv6_perf_front_us / xv6_perf_frames),
                    (long long)(xv6_perf_state_us / xv6_perf_frames),
                    xv6_perf_frames);
            fflush(stderr);
            xv6_perf_frames = 0;
            xv6_perf_thread_us = 0;
            xv6_perf_flush_us = 0;
            xv6_perf_wait_us = 0;
            xv6_perf_front_us = 0;
            xv6_perf_state_us = 0;
            xv6_perf_total_us = 0;
         }
      }
   }
}

static void
drisw_swap_buffers(struct dri_drawable *drawable)
{
   drisw_swap_buffers_with_damage(drawable, 0, NULL);
}

static void
drisw_copy_sub_buffer(struct dri_drawable *drawable, int x, int y,
                      int w, int h)
{
   struct dri_context *ctx = dri_get_current();
   struct dri_screen *screen = drawable->screen;
   struct pipe_resource *ptex;
   struct pipe_box box;
   if (!ctx)
      return;

   ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT];

   if (ptex) {
      /* Wait for glthread to finish because we can't use pipe_context from
       * multiple threads.
       */
      _mesa_glthread_finish(ctx->st->ctx);

      struct pipe_fence_handle *fence = NULL;

      if (!xv6_d3d12_swrast_skip_front_flush())
         st_context_flush(ctx->st, ST_FLUSH_FRONT, &fence, NULL, NULL);

      if (!xv6_d3d12_swrast_skip_prefence())
         screen->base.screen->fence_finish(screen->base.screen, ctx->st->pipe,
                                           fence, OS_TIMEOUT_INFINITE);
      if (fence)
         screen->base.screen->fence_reference(screen->base.screen, &fence, NULL);

      if (drawable->stvis.samples > 1) {
         /* Resolve the back buffer. */
         dri_pipe_blit(ctx->st->pipe,
                       drawable->textures[ST_ATTACHMENT_BACK_LEFT],
                       drawable->msaa_textures[ST_ATTACHMENT_BACK_LEFT]);
      }

      u_box_2d(x, drawable->h - y - h, w, h, &box);
      drisw_present_texture(ctx->st->pipe, drawable, ptex, 1, &box);
   }
}

static bool
drisw_flush_frontbuffer(struct dri_context *ctx,
                        struct dri_drawable *drawable,
                        enum st_attachment_type statt)
{
   struct pipe_resource *ptex;

   if (!ctx || statt != ST_ATTACHMENT_FRONT_LEFT)
      return false;

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   _mesa_glthread_finish(ctx->st->ctx);

   if (drawable->stvis.samples > 1) {
      /* Resolve the front buffer. */
      dri_pipe_blit(ctx->st->pipe,
                    drawable->textures[ST_ATTACHMENT_FRONT_LEFT],
                    drawable->msaa_textures[ST_ATTACHMENT_FRONT_LEFT]);
   }
   ptex = drawable->textures[statt];

   if (ptex) {
      drisw_copy_to_front(ctx->st->pipe, ctx->draw, ptex, 0, NULL);
   }

   return true;
}

extern bool
dri_image_drawable_get_buffers(struct dri_drawable *drawable,
                               struct __DRIimageList *images,
                               const enum st_attachment_type *statts,
                               unsigned statts_count);

static void
handle_in_fence(struct dri_context *ctx, struct dri_image *img)
{
   struct pipe_context *pipe = ctx->st->pipe;
   struct pipe_fence_handle *fence;
   int fd = img->in_fence_fd;

   if (fd == -1)
      return;

   validate_fence_fd(fd);

   img->in_fence_fd = -1;

   pipe->create_fence_fd(pipe, &fence, fd, PIPE_FD_TYPE_NATIVE_SYNC);
   pipe->fence_server_sync(pipe, fence, 0);
   pipe->screen->fence_reference(pipe->screen, &fence, NULL);

   close(fd);
}

/**
 * Allocate framebuffer attachments.
 *
 * During fixed-size operation, the function keeps allocating new attachments
 * as they are requested. Unused attachments are not removed, not until the
 * framebuffer is resized or destroyed.
 */
static void
drisw_allocate_textures(struct dri_context *stctx,
                        struct dri_drawable *drawable,
                        const enum st_attachment_type *statts,
                        unsigned count)
{
   struct dri_screen *screen = drawable->screen;
   const __DRIswrastLoaderExtension *loader = drawable->screen->swrast_loader;
   struct pipe_resource templ;
   unsigned width, height;
   bool resized;
   unsigned i;
   const __DRIimageLoaderExtension *image = screen->image.loader;
   struct __DRIimageList images;
   bool imported_buffers = true;

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   _mesa_glthread_finish(stctx->st->ctx);

   /* First try to get the buffers from the loader */
   if (image) {
      if (!dri_image_drawable_get_buffers(drawable, &images,
                                          statts, count))
         imported_buffers = false;
   }

   width  = drawable->w;
   height = drawable->h;

   resized = (drawable->old_w != width ||
              drawable->old_h != height);

   /* remove outdated textures */
   if (resized) {
      for (i = 0; i < ST_ATTACHMENT_COUNT; i++) {
         pipe_resource_reference(&drawable->textures[i], NULL);
         pipe_resource_reference(&drawable->msaa_textures[i], NULL);
      }
      drawable->buffer_age = 0;
   }

   memset(&templ, 0, sizeof(templ));
   templ.target = screen->target;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;

   if (imported_buffers && image) {
      if (images.image_mask & __DRI_IMAGE_BUFFER_FRONT) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_FRONT_LEFT];
         struct pipe_resource *texture = images.front->texture;

         drawable->w = texture->width0;
         drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);
         handle_in_fence(stctx, images.front);
      }

      if (images.image_mask & __DRI_IMAGE_BUFFER_BACK) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_BACK_LEFT];
         struct pipe_resource *texture = images.back->texture;

         drawable->w = texture->width0;
         drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);
         handle_in_fence(stctx, images.back);
      }

      if (images.image_mask & __DRI_IMAGE_BUFFER_SHARED) {
         struct pipe_resource **buf =
            &drawable->textures[ST_ATTACHMENT_BACK_LEFT];
         struct pipe_resource *texture = images.back->texture;

         drawable->w = texture->width0;
         drawable->h = texture->height0;

         pipe_resource_reference(buf, texture);
         handle_in_fence(stctx, images.back);
      }

      /* Note: if there is both a back and a front buffer,
       * then they have the same size.
       */
      templ.width0 = drawable->w;
      templ.height0 = drawable->h;
   } else {
      for (i = 0; i < count; i++) {
         enum pipe_format format;
         unsigned bind;

         /* the texture already exists or not requested */
         if (drawable->textures[statts[i]])
            continue;

         dri_drawable_get_format(drawable, statts[i], &format, &bind);

         /* if we don't do any present, no need for display targets */
         if (statts[i] != ST_ATTACHMENT_DEPTH_STENCIL && !screen->swrast_no_present)
            bind |= PIPE_BIND_DISPLAY_TARGET;

         if (format == PIPE_FORMAT_NONE)
            continue;

         templ.format = format;
         templ.bind = bind;
         templ.nr_samples = 0;
         templ.nr_storage_samples = 0;

         if (statts[i] == ST_ATTACHMENT_FRONT_LEFT &&
                    screen->base.screen->resource_create_front &&
                    loader->base.version >= 3) {
            drawable->textures[statts[i]] =
               screen->base.screen->resource_create_front(screen->base.screen, &templ, (const void *)drawable);
         } else
            drawable->textures[statts[i]] =
               screen->base.screen->resource_create(screen->base.screen, &templ);

         if (drawable->stvis.samples > 1) {
            templ.bind = templ.bind &
               ~(PIPE_BIND_SCANOUT | PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET);
            templ.nr_samples = drawable->stvis.samples;
            templ.nr_storage_samples = drawable->stvis.samples;
            drawable->msaa_textures[statts[i]] =
               screen->base.screen->resource_create(screen->base.screen, &templ);

            dri_pipe_blit(stctx->st->pipe,
                          drawable->msaa_textures[statts[i]],
                          drawable->textures[statts[i]]);
         }
      }
   }

   drawable->old_w = width;
   drawable->old_h = height;
}

void
drisw_update_tex_buffer(struct dri_drawable *drawable,
                        struct dri_context *ctx,
                        struct pipe_resource *res)
{
   struct st_context *st_ctx = (struct st_context *)ctx->st;
   struct pipe_context *pipe = st_ctx->pipe;
   struct pipe_transfer *transfer;
   char *map;
   int x, y, w, h;
   int ximage_stride, line;
   int cpp = util_format_get_blocksize(res->format);

   /* Wait for glthread to finish because we can't use pipe_context from
    * multiple threads.
    */
   _mesa_glthread_finish(ctx->st->ctx);

   get_drawable_info(drawable, &x, &y, &w, &h);

   map = pipe_texture_map(pipe, res,
                           0, 0, // level, layer,
                           PIPE_MAP_WRITE,
                           x, y, w, h, &transfer);

   /* Copy the Drawable content to the mapped texture buffer */
   if (!get_image_shm(drawable, x, y, w, h, res))
      get_image(drawable, x, y, w, h, map);

   /* The pipe transfer has a pitch rounded up to the nearest 64 pixels.
      get_image() has a pitch rounded up to 4 bytes.  */
   ximage_stride = ((w * cpp) + 3) & -4;
   for (line = h-1; line; --line) {
      memmove(&map[line * transfer->stride],
              &map[line * ximage_stride],
              ximage_stride);
   }

   pipe_texture_unmap(pipe, transfer);
}

/*
 * Backend function for init_screen.
 */

static const struct drisw_loader_funcs drisw_lf = {
   .get_image = drisw_get_image,
   .put_image = drisw_put_image,
   .put_image2 = drisw_put_image2
};

static const struct drisw_loader_funcs drisw_shm_lf = {
   .get_image = drisw_get_image,
   .put_image = drisw_put_image,
   .put_image2 = drisw_put_image2,
   .put_image_shm = drisw_put_image_shm
};

void
drisw_init_drawable(struct dri_drawable *drawable, bool isPixmap, int alphaBits)
{
   drawable->allocate_textures = drisw_allocate_textures;
   drawable->update_drawable_info = drisw_update_drawable_info;
   drawable->flush_frontbuffer = drisw_flush_frontbuffer;
   drawable->update_tex_buffer = drisw_update_tex_buffer;
   drawable->swap_buffers = drisw_swap_buffers;
   drawable->swap_buffers_with_damage = drisw_swap_buffers_with_damage;
}

struct pipe_screen *
drisw_init_screen(struct dri_screen *screen, bool driver_name_is_inferred)
{
   const __DRIswrastLoaderExtension *loader = screen->swrast_loader;
   struct pipe_screen *pscreen = NULL;
   const struct drisw_loader_funcs *lf = &drisw_lf;

   fprintf(stderr,
           "xv6-mesa: drisw_init_screen fd=%d loader=%p version=%u inferred=%d\n",
           screen->fd, (void *)loader, loader ? loader->base.version : 0,
           driver_name_is_inferred);
   screen->swrast_no_present = debug_get_option_swrast_no_present();

   if (loader->base.version >= 4) {
      if (loader->putImageShm)
         lf = &drisw_shm_lf;
   }

   bool success = false;
#ifdef HAVE_DRISW_KMS
   if (screen->fd != -1)
      success = pipe_loader_sw_probe_kms(&screen->dev, screen->fd);
#endif
   if (!success)
      success = pipe_loader_sw_probe_dri(&screen->dev, lf);

   fprintf(stderr, "xv6-mesa: drisw pipe_loader success=%d dev=%p\n",
           success, (void *)screen->dev);
   if (success)
      pscreen = pipe_loader_create_screen(screen->dev, driver_name_is_inferred);

   fprintf(stderr, "xv6-mesa: drisw pscreen=%p\n", (void *)pscreen);
   return pscreen;
}

/* swrast copy sub buffer entrypoint. */
void
driswCopySubBuffer(struct dri_drawable *drawable, int x, int y, int w, int h)
{
   assert(drawable->screen->swrast_loader);

   drisw_copy_sub_buffer(drawable, x, y, w, h);
}

/* vim: set sw=3 ts=8 sts=3 expandtab: */
