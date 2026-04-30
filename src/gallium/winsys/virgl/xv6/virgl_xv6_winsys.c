/*
 * Copyright 2026 ElectronSpark
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_thread.h"
#include "virgl/virgl_winsys.h"
#include "virgl_resource_cache.h"
#include "virgl_xv6_public.h"

#define FB_GPU_VIRGL_CTX_CREATE  0x4619
#define FB_GPU_VIRGL_CTX_DESTROY 0x461A
#define FB_GPU_VIRGL_SUBMIT      0x461B
#define FB_GPU_VIRGL_FENCE       0x461C
#define FB_GPU_VIRGL_GET_CAPS    0x461D
#define FB_GPU_VIRGL_RESOURCE_CREATE 0x461E
#define FB_GPU_VIRGL_RESOURCE_DESTROY 0x461F
#define FB_GPU_VIRGL_TRANSFER_TO_HOST 0x4620
#define FB_GPU_VIRGL_TRANSFER_FROM_HOST 0x4621
#define FB_GPU_VIRGL_FENCE_WAIT 0x1

struct fb_gpu_virgl_ctx {
   uint32_t ctx_id;
   uint32_t flags;
   char debug_name[64];
};

struct fb_gpu_virgl_submit {
   uint32_t ctx_id;
   uint32_t flags;
   uint32_t cmd_size;
   uint32_t reserved;
   uint64_t cmd;
   uint64_t fence;
   uint64_t signaled;
};

struct fb_gpu_virgl_fence {
   uint32_t flags;
   uint32_t padding;
   uint64_t wait_for;
   uint64_t signaled;
};

struct fb_gpu_virgl_caps {
   uint32_t flags;
   uint32_t capset_id;
   uint32_t capset_version;
   uint32_t size;
   uint64_t data;
};

struct fb_gpu_virgl_resource_create {
   uint32_t ctx_id;
   uint32_t flags;
   uint32_t resource_id;
   uint32_t target;
   uint32_t format;
   uint32_t bind;
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t array_size;
   uint32_t last_level;
   uint32_t nr_samples;
   uint64_t size;
   uint64_t addr;
};

struct fb_gpu_virgl_resource_destroy {
   uint32_t resource_id;
   uint32_t flags;
};

struct fb_gpu_virgl_transfer {
   uint32_t resource_id;
   uint32_t flags;
   uint32_t x;
   uint32_t y;
   uint32_t z;
   uint32_t w;
   uint32_t h;
   uint32_t d;
   uint64_t offset;
   uint32_t level;
   uint32_t stride;
   uint32_t layer_stride;
   uint32_t padding;
};

struct virgl_hw_res {
   struct pipe_reference reference;
   uint32_t res_handle;
   int num_cs_references;
   uint32_t size;
   void *ptr;
   uint32_t bind;
   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t last_level;
   struct virgl_resource_cache_entry cache_entry;
};

struct virgl_xv6_winsys {
   struct virgl_winsys base;
   int fd;
   uint32_t ctx_id;
   mtx_t mutex;
};

struct virgl_xv6_fence {
   struct pipe_reference reference;
   uint64_t fence_id;
};

struct virgl_xv6_cmd_buf {
   struct virgl_cmd_buf base;
   uint32_t *buf;
   unsigned nres;
   unsigned cres;
   struct virgl_hw_res **res_bo;
   struct virgl_winsys *ws;
};

static inline struct virgl_xv6_winsys *
virgl_xv6_winsys(struct virgl_winsys *vws)
{
   return (struct virgl_xv6_winsys *)vws;
}

static inline struct virgl_xv6_cmd_buf *
virgl_xv6_cmd_buf(struct virgl_cmd_buf *cbuf)
{
   return (struct virgl_xv6_cmd_buf *)cbuf;
}

static inline struct virgl_xv6_fence *
virgl_xv6_fence(struct pipe_fence_handle *fence)
{
   return (struct virgl_xv6_fence *)fence;
}

static bool
virgl_xv6_debug_enabled(void)
{
   const char *env = getenv("XV6_VIRGL_DEBUG");
   return env && env[0] && strcmp(env, "0") != 0;
}

static void
virgl_xv6_hw_res_destroy(struct virgl_xv6_winsys *xws,
                         struct virgl_hw_res *res)
{
   if (!res)
      return;

   if (res->ptr && res->size)
      munmap(res->ptr, res->size);
   if (res->res_handle) {
      struct fb_gpu_virgl_resource_destroy destroy = {
         .resource_id = res->res_handle,
      };
      ioctl(xws->fd, FB_GPU_VIRGL_RESOURCE_DESTROY, &destroy);
   }
   FREE(res);
}

static void
virgl_xv6_resource_reference(struct virgl_winsys *vws,
                             struct virgl_hw_res **dres,
                             struct virgl_hw_res *sres)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(old ? &old->reference : NULL,
                      sres ? &sres->reference : NULL))
      virgl_xv6_hw_res_destroy(xws, old);
   *dres = sres;
}

static struct virgl_hw_res *
virgl_xv6_resource_create(struct virgl_winsys *vws,
                          enum pipe_texture_target target,
                          const void *map_front_private,
                          uint32_t format, uint32_t bind,
                          uint32_t width, uint32_t height,
                          uint32_t depth, uint32_t array_size,
                          uint32_t last_level, uint32_t nr_samples,
                          uint32_t flags, uint32_t size)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct virgl_hw_res *res = CALLOC_STRUCT(virgl_hw_res);
   struct fb_gpu_virgl_resource_create create;

   (void)map_front_private;
   if (!res)
      return NULL;

   memset(&create, 0, sizeof(create));
   create.ctx_id = xws->ctx_id;
   create.flags = flags;
   create.target = target;
   create.format = pipe_to_virgl_format(format);
   create.bind = bind;
   create.width = width;
   create.height = height ? height : 1;
   create.depth = depth ? depth : 1;
   create.array_size = array_size ? array_size : 1;
   create.last_level = last_level;
   create.nr_samples = nr_samples;
   create.size = size;

   if (ioctl(xws->fd, FB_GPU_VIRGL_RESOURCE_CREATE, &create) < 0 ||
       create.resource_id == 0 || create.addr == 0 || create.size == 0) {
      if (virgl_xv6_debug_enabled())
         fprintf(stderr,
                 "virgl-xv6: resource create failed target=%u format=%u bind=0x%x %ux%u size=%u\n",
                 target, format, bind, width, height, size);
      FREE(res);
      return NULL;
   }
   if (virgl_xv6_debug_enabled())
      fprintf(stderr,
              "virgl-xv6: resource id=%u target=%u format=%u virgl_format=%u bind=0x%x %ux%u size=%u map=%p\n",
              create.resource_id, target, format, create.format, bind, width,
              height, (uint32_t)create.size, (void *)(uintptr_t)create.addr);

   res->res_handle = create.resource_id;
   res->size = create.size;
   res->ptr = (void *)(uintptr_t)create.addr;
   res->bind = bind;
   res->format = format;
   res->width = width;
   res->height = height ? height : 1;
   res->depth = depth ? depth : 1;
   res->last_level = last_level;
   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->num_cs_references, 0);
   return res;
}

static void *
virgl_xv6_resource_map(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   (void)vws;
   return res->ptr;
}

static void
virgl_xv6_resource_wait(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   (void)vws;
   (void)res;
}

static bool
virgl_xv6_resource_is_busy(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   (void)vws;
   (void)res;
   return false;
}

static int
virgl_xv6_transfer(struct virgl_winsys *vws, struct virgl_hw_res *res,
                   const struct pipe_box *box, uint32_t stride,
                   uint32_t layer_stride, uint32_t buf_offset,
                   uint32_t level, int from_host)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct fb_gpu_virgl_transfer transfer;

   memset(&transfer, 0, sizeof(transfer));
   transfer.resource_id = res->res_handle;
   transfer.x = box->x;
   transfer.y = box->y;
   transfer.z = box->z;
   transfer.w = box->width;
   transfer.h = box->height;
   transfer.d = box->depth;
   transfer.offset = buf_offset;
   transfer.level = level;
   transfer.stride = stride;
   transfer.layer_stride = layer_stride;

   return ioctl(xws->fd, from_host ? FB_GPU_VIRGL_TRANSFER_FROM_HOST :
                                     FB_GPU_VIRGL_TRANSFER_TO_HOST,
                &transfer);
}

static int
virgl_xv6_transfer_put(struct virgl_winsys *vws, struct virgl_hw_res *res,
                       const struct pipe_box *box, uint32_t stride,
                       uint32_t layer_stride, uint32_t buf_offset,
                       uint32_t level)
{
   return virgl_xv6_transfer(vws, res, box, stride, layer_stride, buf_offset,
                             level, 0);
}

static int
virgl_xv6_transfer_get(struct virgl_winsys *vws, struct virgl_hw_res *res,
                       const struct pipe_box *box, uint32_t stride,
                       uint32_t layer_stride, uint32_t buf_offset,
                       uint32_t level)
{
   return virgl_xv6_transfer(vws, res, box, stride, layer_stride, buf_offset,
                             level, 1);
}

static struct virgl_cmd_buf *
virgl_xv6_cmd_buf_create(struct virgl_winsys *vws, uint32_t size)
{
   struct virgl_xv6_cmd_buf *cbuf = CALLOC_STRUCT(virgl_xv6_cmd_buf);
   if (!cbuf)
      return NULL;

   cbuf->nres = 512;
   cbuf->res_bo = CALLOC(cbuf->nres, sizeof(*cbuf->res_bo));
   cbuf->buf = CALLOC(size, sizeof(uint32_t));
   if (!cbuf->res_bo || !cbuf->buf) {
      FREE(cbuf->res_bo);
      FREE(cbuf->buf);
      FREE(cbuf);
      return NULL;
   }

   cbuf->ws = vws;
   cbuf->base.buf = cbuf->buf;
   return &cbuf->base;
}

static void
virgl_xv6_release_all_res(struct virgl_xv6_winsys *xws,
                          struct virgl_xv6_cmd_buf *cbuf)
{
   for (unsigned i = 0; i < cbuf->cres; i++) {
      p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
      virgl_xv6_resource_reference(&xws->base, &cbuf->res_bo[i], NULL);
   }
   cbuf->cres = 0;
}

static void
virgl_xv6_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct virgl_xv6_cmd_buf *cbuf = virgl_xv6_cmd_buf(_cbuf);
   virgl_xv6_release_all_res(virgl_xv6_winsys(cbuf->ws), cbuf);
   FREE(cbuf->res_bo);
   FREE(cbuf->buf);
   FREE(cbuf);
}

static bool
virgl_xv6_res_is_added(struct virgl_xv6_cmd_buf *cbuf,
                       struct virgl_hw_res *res)
{
   for (unsigned i = 0; i < cbuf->cres; i++) {
      if (cbuf->res_bo[i] == res)
         return true;
   }
   return false;
}

static void
virgl_xv6_add_res(struct virgl_xv6_winsys *xws,
                  struct virgl_xv6_cmd_buf *cbuf,
                  struct virgl_hw_res *res)
{
   if (virgl_xv6_res_is_added(cbuf, res))
      return;

   if (cbuf->cres >= cbuf->nres) {
      unsigned new_nres = cbuf->nres + 256;
      struct virgl_hw_res **new_res =
         REALLOC(cbuf->res_bo, cbuf->nres * sizeof(*cbuf->res_bo),
                 new_nres * sizeof(*cbuf->res_bo));
      if (!new_res)
         return;
      cbuf->res_bo = new_res;
      cbuf->nres = new_nres;
   }

   cbuf->res_bo[cbuf->cres] = NULL;
   virgl_xv6_resource_reference(&xws->base, &cbuf->res_bo[cbuf->cres], res);
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static void
virgl_xv6_emit_res(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                   struct virgl_hw_res *res, bool write_buffer)
{
   struct virgl_xv6_cmd_buf *cbuf = virgl_xv6_cmd_buf(_cbuf);

   if (write_buffer)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   virgl_xv6_add_res(virgl_xv6_winsys(vws), cbuf, res);
}

static bool
virgl_xv6_res_is_referenced(struct virgl_winsys *vws,
                            struct virgl_cmd_buf *cbuf,
                            struct virgl_hw_res *res)
{
   (void)vws;
   (void)cbuf;
   return p_atomic_read(&res->num_cs_references) != 0;
}

static struct pipe_fence_handle *
virgl_xv6_fence_create(uint64_t fence_id)
{
   struct virgl_xv6_fence *fence = CALLOC_STRUCT(virgl_xv6_fence);
   if (!fence)
      return NULL;

   pipe_reference_init(&fence->reference, 1);
   fence->fence_id = fence_id;
   return (struct pipe_fence_handle *)fence;
}

static int
virgl_xv6_submit_cmd(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                     struct pipe_fence_handle **fence)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct virgl_xv6_cmd_buf *cbuf = virgl_xv6_cmd_buf(_cbuf);
   struct fb_gpu_virgl_submit submit;
   int ret;

   if (cbuf->base.cdw == 0)
      return 0;

   memset(&submit, 0, sizeof(submit));
   submit.ctx_id = xws->ctx_id;
   submit.cmd = (uint64_t)(uintptr_t)cbuf->base.buf;
   submit.cmd_size = cbuf->base.cdw * sizeof(uint32_t);

   ret = ioctl(xws->fd, FB_GPU_VIRGL_SUBMIT, &submit);
   if (virgl_xv6_debug_enabled())
      fprintf(stderr, "virgl-xv6: submit cdw=%u bytes=%u ret=%d fence=%lu signaled=%lu\n",
              cbuf->base.cdw, submit.cmd_size, ret,
              (unsigned long)submit.fence, (unsigned long)submit.signaled);
   if (fence && ret == 0)
      *fence = virgl_xv6_fence_create(submit.fence);

   virgl_xv6_release_all_res(xws, cbuf);
   cbuf->base.cdw = 0;
   return ret;
}

static int
virgl_xv6_get_caps(struct virgl_winsys *vws, struct virgl_drm_caps *caps)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct fb_gpu_virgl_caps req;

   memset(caps, 0, sizeof(*caps));
   virgl_ws_fill_new_caps_defaults(caps);
   memset(&req, 0, sizeof(req));
   req.data = (uint64_t)(uintptr_t)&caps->caps;
   req.size = sizeof(caps->caps);

   int ret = ioctl(xws->fd, FB_GPU_VIRGL_GET_CAPS, &req);
   if (virgl_xv6_debug_enabled())
      fprintf(stderr, "virgl-xv6: caps ret=%d id=%u version=%u size=%u\n",
              ret, req.capset_id, req.capset_version, req.size);
   return ret;
}

static struct pipe_fence_handle *
virgl_xv6_cs_create_fence(struct virgl_winsys *vws, int fd)
{
   (void)vws;
   (void)fd;
   return virgl_xv6_fence_create(0);
}

static bool
virgl_xv6_fence_wait(struct virgl_winsys *vws,
                     struct pipe_fence_handle *fence,
                     uint64_t timeout)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct virgl_xv6_fence *xfence = virgl_xv6_fence(fence);
   struct fb_gpu_virgl_fence req;

   (void)timeout;
   if (xfence->fence_id == 0)
      return true;

   memset(&req, 0, sizeof(req));
   req.flags = FB_GPU_VIRGL_FENCE_WAIT;
   req.wait_for = xfence->fence_id;
   return ioctl(xws->fd, FB_GPU_VIRGL_FENCE, &req) == 0 &&
          req.signaled >= xfence->fence_id;
}

static void
virgl_xv6_fence_reference(struct virgl_winsys *vws,
                          struct pipe_fence_handle **dst,
                          struct pipe_fence_handle *src)
{
   struct virgl_xv6_fence *old = virgl_xv6_fence(*dst);
   struct virgl_xv6_fence *newf = virgl_xv6_fence(src);

   (void)vws;
   if (pipe_reference(old ? &old->reference : NULL,
                      newf ? &newf->reference : NULL))
      FREE(old);
   *dst = src;
}

static uint32_t
virgl_xv6_resource_get_storage_size(struct virgl_winsys *vws,
                                    struct virgl_hw_res *res)
{
   (void)vws;
   return res->size;
}

static void
virgl_xv6_resource_set_type(struct virgl_winsys *vws,
                            struct virgl_hw_res *res,
                            uint32_t format, uint32_t bind,
                            uint32_t width, uint32_t height,
                            uint32_t usage, uint64_t modifier,
                            uint32_t plane_count,
                            const uint32_t *plane_strides,
                            const uint32_t *plane_offsets)
{
   (void)vws;
   (void)usage;
   (void)modifier;
   (void)plane_count;
   (void)plane_strides;
   (void)plane_offsets;
   res->format = format;
   res->bind = bind;
   res->width = width;
   res->height = height;
}

static void
virgl_xv6_flush_frontbuffer(struct virgl_winsys *vws,
                            struct virgl_cmd_buf *cbuf,
                            struct virgl_hw_res *res,
                            unsigned level, unsigned layer,
                            void *winsys_drawable_handle,
                            struct pipe_box *sub_box)
{
   (void)vws;
   (void)cbuf;
   (void)res;
   (void)level;
   (void)layer;
   (void)winsys_drawable_handle;
   (void)sub_box;
}

static void
virgl_xv6_destroy(struct virgl_winsys *vws)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct fb_gpu_virgl_ctx ctx = {
      .ctx_id = xws->ctx_id,
   };

   if (xws->ctx_id)
      ioctl(xws->fd, FB_GPU_VIRGL_CTX_DESTROY, &ctx);
   if (xws->fd >= 0)
      close(xws->fd);
   mtx_destroy(&xws->mutex);
   FREE(xws);
}

struct virgl_winsys *
virgl_xv6_winsys_create(void)
{
   struct virgl_xv6_winsys *xws = CALLOC_STRUCT(virgl_xv6_winsys);
   struct fb_gpu_virgl_ctx ctx;
   struct virgl_drm_caps caps;

   if (!xws)
      return NULL;

   xws->fd = open("/dev/gpu0", O_RDWR);
   if (xws->fd < 0)
      xws->fd = open("/dev/fb0", O_RDWR);
   if (xws->fd < 0)
      goto fail;

   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.debug_name, sizeof(ctx.debug_name), "mesa-virgl");
   if (ioctl(xws->fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0)
      goto fail;
   xws->ctx_id = ctx.ctx_id;

   (void)mtx_init(&xws->mutex, mtx_plain);

   xws->base.destroy = virgl_xv6_destroy;
   xws->base.get_fd = NULL;
   xws->base.transfer_put = virgl_xv6_transfer_put;
   xws->base.transfer_get = virgl_xv6_transfer_get;
   xws->base.resource_create = virgl_xv6_resource_create;
   xws->base.resource_reference = virgl_xv6_resource_reference;
   xws->base.resource_map = virgl_xv6_resource_map;
   xws->base.resource_wait = virgl_xv6_resource_wait;
   xws->base.resource_is_busy = virgl_xv6_resource_is_busy;
   xws->base.resource_create_from_handle = NULL;
   xws->base.resource_set_type = virgl_xv6_resource_set_type;
   xws->base.resource_get_handle = NULL;
   xws->base.resource_get_storage_size = virgl_xv6_resource_get_storage_size;
   xws->base.cmd_buf_create = virgl_xv6_cmd_buf_create;
   xws->base.cmd_buf_destroy = virgl_xv6_cmd_buf_destroy;
   xws->base.emit_res = virgl_xv6_emit_res;
   xws->base.submit_cmd = virgl_xv6_submit_cmd;
   xws->base.res_is_referenced = virgl_xv6_res_is_referenced;
   xws->base.get_caps = virgl_xv6_get_caps;
   xws->base.cs_create_fence = virgl_xv6_cs_create_fence;
   xws->base.fence_wait = virgl_xv6_fence_wait;
   xws->base.fence_reference = virgl_xv6_fence_reference;
   xws->base.supports_fences = 0;
   xws->base.supports_encoded_transfers = 0;
   xws->base.supports_coherent = 1;
   xws->base.flush_frontbuffer = virgl_xv6_flush_frontbuffer;

   if (virgl_xv6_get_caps(&xws->base, &caps) != 0)
      goto fail;

   return &xws->base;

fail:
   if (xws->ctx_id) {
      struct fb_gpu_virgl_ctx destroy = {
         .ctx_id = xws->ctx_id,
      };
      ioctl(xws->fd, FB_GPU_VIRGL_CTX_DESTROY, &destroy);
   }
   if (xws->fd >= 0)
      close(xws->fd);
   FREE(xws);
   return NULL;
}
