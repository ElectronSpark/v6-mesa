/*
 * Copyright 2026 ElectronSpark
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_thread.h"
#include "virtio-gpu/virgl_hw.h"
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
#define FB_GPU_BO_DESTROY 0x4616
#define FB_GPU_BO_IMPORT_FD 0x4623
#define FB_GPU_VIRGL_FENCE_EXPORT_FD 0x4626
#define FB_GPU_VIRGL_FENCE_QUERY_FD 0x4627
#define FB_GPU_VIRGL_RESOURCE_EXPORT_FD 0x4628
#define FB_GPU_BO_INFO 0x462E
#define FB_GPU_VIRGL_RESOURCE_ATTACH 0x4634
#define FB_GPU_VIRGL_FENCE_WAIT 0x1
#define FB_GPU_VIRGL_SUBMIT_ASYNC 0x1
#define FB_GPU_VIRGL_SUBMIT_FORCE_FAIL 0x80000000u

struct fb_gpu_virgl_ctx {
   uint32_t ctx_id;
   uint32_t flags;
   char debug_name[64];
};

struct fb_gpu_virgl_submit {
   uint32_t ctx_id;
   uint32_t flags;
   uint32_t cmd_size;
   uint32_t resource_count;
   uint64_t cmd;
   uint64_t fence;
   uint64_t signaled;
   uint64_t resources;
};

struct fb_gpu_virgl_fence {
   uint32_t flags;
   uint32_t padding;
   uint64_t wait_for;
   uint64_t signaled;
};

struct fb_gpu_virgl_fence_export_fd {
   uint32_t flags;
   int32_t fd;
   uint64_t fence;
   uint64_t signaled;
};

struct fb_gpu_virgl_fence_query_fd {
   int32_t fd;
   uint32_t flags;
   uint64_t fence;
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

struct fb_gpu_virgl_resource_attach {
   uint32_t ctx_id;
   uint32_t resource_id;
   uint32_t handle;
   uint32_t flags;
};

struct fb_gpu_virgl_resource_export_fd {
   uint32_t resource_id;
   uint32_t flags;
   int32_t fd;
   uint32_t handle;
   uint32_t width;
   uint32_t height;
   uint32_t pitch;
   uint32_t reserved;
   uint64_t size;
};

struct fb_gpu_bo_destroy {
   uint32_t handle;
   uint32_t flags;
};

struct fb_gpu_bo_import_fd {
   int32_t fd;
   uint32_t flags;
   uint32_t width;
   uint32_t height;
   uint32_t pitch;
   uint32_t handle;
   uint64_t size;
   uint64_t addr;
   uint32_t format;
   uint32_t plane_count;
   uint64_t modifier;
   uint32_t offsets[4];
   uint32_t strides[4];
   uint64_t implicit_fence;
   uint64_t explicit_fence;
};

struct fb_gpu_bo_info {
   uint32_t handle;
   uint32_t flags;
   uint32_t width;
   uint32_t height;
   uint32_t pitch;
   uint32_t format;
   uint64_t modifier;
   uint64_t size;
   uint64_t addr_align;
   uint64_t size_align;
   uint32_t page_size;
   uint32_t reserved;
   uint64_t mmap_offset;
   uint32_t plane_count;
   uint32_t metadata_flags;
   uint32_t offsets[4];
   uint32_t strides[4];
   uint64_t implicit_fence;
   uint64_t explicit_fence;
   uint32_t virtio_resource_id;
   uint32_t reserved1;
   uint64_t virtio_resource_owner_id;
   int32_t virtio_resource_owner_tgid;
   uint32_t reserved2;
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
   uint32_t bo_handle;
   bool imported;
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
   void *kms_ptr;
   uint64_t kms_size;
};

struct virgl_xv6_winsys {
   struct virgl_winsys base;
   int fd;
   uint32_t ctx_id;
   bool context_lost;
   bool sync_submit;
   bool force_loss_triggered;
   uint64_t force_loss_after_seconds;
   uint64_t force_loss_start_nsec;
   mtx_t mutex;
};

struct virgl_xv6_fence {
   struct pipe_reference reference;
   bool external;
   int fd;
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

static bool
virgl_xv6_env_enabled(const char *name)
{
   const char *env = getenv(name);

   return env && env[0] && strcmp(env, "0") != 0 &&
          strcmp(env, "false") != 0 && strcmp(env, "no") != 0;
}

static bool
virgl_xv6_perf_enabled(void)
{
   const char *env = getenv("XV6_MESA_PERF_LOG");

   return env && env[0] && strcmp(env, "0") != 0 &&
          strcmp(env, "false") != 0;
}

static int64_t
virgl_xv6_now_us(void)
{
   struct timespec ts;

   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void
virgl_xv6_perf_note_submit(int64_t us, uint32_t bytes, uint32_t resources,
                           int ret)
{
   static uint64_t calls;
   static uint64_t ret_failures;
   static uint64_t bytes_total;
   static uint64_t resources_total;
   static int64_t us_total;
   static int64_t us_max;

   if (!virgl_xv6_perf_enabled())
      return;
   calls++;
   if (ret != 0)
      ret_failures++;
   bytes_total += bytes;
   resources_total += resources;
   us_total += us;
   if (us > us_max)
      us_max = us;
   if (calls < 60)
      return;
   fprintf(stderr,
           "virgl-xv6: perf submit avg_us=%lld max_us=%lld calls=%lu "
           "bytes_avg=%lu resources_avg=%lu failures=%lu\n",
           (long long)(us_total / (int64_t)calls), (long long)us_max,
           (unsigned long)calls, (unsigned long)(bytes_total / calls),
           (unsigned long)(resources_total / calls),
           (unsigned long)ret_failures);
   fflush(stderr);
   calls = 0;
   ret_failures = 0;
   bytes_total = 0;
   resources_total = 0;
   us_total = 0;
   us_max = 0;
}

static void
virgl_xv6_perf_note_wait(int64_t us, bool fd_path, bool ok, uint64_t timeout)
{
   static uint64_t calls;
   static uint64_t fd_calls;
   static uint64_t failures;
   static uint64_t infinite;
   static int64_t us_total;
   static int64_t us_max;

   if (!virgl_xv6_perf_enabled())
      return;
   calls++;
   if (fd_path)
      fd_calls++;
   if (!ok)
      failures++;
   if (timeout == OS_TIMEOUT_INFINITE)
      infinite++;
   us_total += us;
   if (us > us_max)
      us_max = us;
   if (calls < 60)
      return;
   fprintf(stderr,
           "virgl-xv6: perf fence_wait avg_us=%lld max_us=%lld calls=%lu "
           "fd_calls=%lu infinite=%lu failures=%lu\n",
           (long long)(us_total / (int64_t)calls), (long long)us_max,
           (unsigned long)calls, (unsigned long)fd_calls,
           (unsigned long)infinite, (unsigned long)failures);
   fflush(stderr);
   calls = 0;
   fd_calls = 0;
   failures = 0;
   infinite = 0;
   us_total = 0;
   us_max = 0;
}

static void
virgl_xv6_perf_note_transfer(int64_t us, bool from_host, int ret,
                             uint32_t width, uint32_t height)
{
   static uint64_t calls;
   static uint64_t from_host_calls;
   static uint64_t failures;
   static uint64_t pixels_total;
   static int64_t us_total;
   static int64_t us_max;

   if (!virgl_xv6_perf_enabled())
      return;
   calls++;
   if (from_host)
      from_host_calls++;
   if (ret != 0)
      failures++;
   pixels_total += (uint64_t)width * height;
   us_total += us;
   if (us > us_max)
      us_max = us;
   if (calls < 60)
      return;
   fprintf(stderr,
           "virgl-xv6: perf transfer avg_us=%lld max_us=%lld calls=%lu "
           "from_host=%lu pixels_avg=%lu failures=%lu\n",
           (long long)(us_total / (int64_t)calls), (long long)us_max,
           (unsigned long)calls, (unsigned long)from_host_calls,
           (unsigned long)(pixels_total / calls), (unsigned long)failures);
   fflush(stderr);
   calls = 0;
   from_host_calls = 0;
   failures = 0;
   pixels_total = 0;
   us_total = 0;
   us_max = 0;
}

static bool
virgl_xv6_native_fences_enabled(void)
{
   const char *env = getenv("XV6_VIRGL_NATIVE_FENCES");

   /*
    * Keep the fence-fd plumbing available for explicit-sync bring-up, but do
    * not advertise native fences by default yet.  The Wayland dmabuf path in
    * this port does not currently hand those fences to wlcomp as acquire
    * fences, so enabling them can add waits without improving presentation.
    */
   return env && env[0] && strcmp(env, "0") != 0;
}

static void
virgl_xv6_mark_context_lost(struct virgl_xv6_winsys *xws, const char *reason,
                            int err)
{
   bool first;

   mtx_lock(&xws->mutex);
   first = !xws->context_lost;
   xws->context_lost = true;
   mtx_unlock(&xws->mutex);

   if (first)
      fprintf(stderr, "virgl-xv6: context %u lost after %s errno=%d\n",
              xws->ctx_id, reason ? reason : "gpu error", err);
}

static bool
virgl_xv6_context_lost(struct virgl_xv6_winsys *xws)
{
   bool lost;

   mtx_lock(&xws->mutex);
   lost = xws->context_lost;
   mtx_unlock(&xws->mutex);
   return lost;
}

static uint64_t
virgl_xv6_env_u64(const char *name)
{
   const char *env = getenv(name);
   char *end = NULL;
   unsigned long long value;

   if (!env || !env[0] || strcmp(env, "0") == 0)
      return 0;
   errno = 0;
   value = strtoull(env, &end, 10);
   if (errno != 0 || end == env)
      return 0;
   return (uint64_t)value;
}

static uint64_t
virgl_xv6_now_nsec(void)
{
   struct timespec ts;

   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool
virgl_xv6_should_force_context_loss(struct virgl_xv6_winsys *xws)
{
   bool force = false;
   uint64_t elapsed_seconds = 0;
   uint64_t now = 0;

   mtx_lock(&xws->mutex);
   if (xws->force_loss_after_seconds != 0 && !xws->force_loss_triggered) {
      now = virgl_xv6_now_nsec();
      if (now != 0) {
         if (xws->force_loss_start_nsec == 0)
            xws->force_loss_start_nsec = now;
         elapsed_seconds =
            (now - xws->force_loss_start_nsec) / 1000000000ull;
         if (elapsed_seconds >= xws->force_loss_after_seconds) {
            xws->force_loss_triggered = true;
            force = true;
         }
      }
   }
   mtx_unlock(&xws->mutex);

   if (force)
      fprintf(stderr,
              "virgl-xv6: forcing context %u loss after %lu seconds\n",
              xws->ctx_id, (unsigned long)elapsed_seconds);
   return force;
}

static int
virgl_xv6_dupfd_cloexec(int fd)
{
   int dupfd;

   if (fd < 0)
      return -1;
#ifdef F_DUPFD_CLOEXEC
   dupfd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (dupfd >= 0)
      return dupfd;
#endif
   dupfd = dup(fd);
   if (dupfd >= 0) {
      int flags = fcntl(dupfd, F_GETFD);
      if (flags >= 0)
         (void)fcntl(dupfd, F_SETFD, flags | FD_CLOEXEC);
   }
   return dupfd;
}

static int
virgl_xv6_timeout_to_ms(uint64_t timeout)
{
   uint64_t timeout_ms;

   if (timeout == 0)
      return 0;
   if (timeout == OS_TIMEOUT_INFINITE)
      return -1;
   timeout_ms = timeout / 1000000;
   if (timeout_ms * 1000000 < timeout)
      timeout_ms++;
   return timeout_ms > INT_MAX ? -1 : (int)timeout_ms;
}

static bool
virgl_xv6_wait_fd(int fd, uint64_t timeout)
{
   struct pollfd pfd = {
      .fd = fd,
      .events = POLLIN,
   };
   int timeout_ms = virgl_xv6_timeout_to_ms(timeout);
   int ret;

   do {
      ret = poll(&pfd, 1, timeout_ms);
   } while (ret < 0 && errno == EINTR);

   return ret > 0 &&
          (pfd.revents & (POLLIN | POLLRDNORM | POLLERR | POLLHUP)) != 0;
}

static void
virgl_xv6_hw_res_destroy(struct virgl_xv6_winsys *xws,
                         struct virgl_hw_res *res)
{
   if (!res)
      return;

   if (res->kms_ptr && res->kms_size)
      munmap(res->kms_ptr, res->kms_size);
   if (!res->imported && res->bo_handle) {
      struct fb_gpu_bo_destroy destroy = {
         .handle = res->bo_handle,
      };
      ioctl(xws->fd, FB_GPU_BO_DESTROY, &destroy);
   }
   if (res->ptr && res->size)
      munmap(res->ptr, res->size);
   if (res->imported && res->bo_handle) {
      struct fb_gpu_bo_destroy destroy = {
         .handle = res->bo_handle,
      };
      ioctl(xws->fd, FB_GPU_BO_DESTROY, &destroy);
   } else if (res->res_handle) {
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
   struct virgl_hw_res *res;
   struct fb_gpu_virgl_resource_create create;

   (void)map_front_private;
   if (virgl_xv6_context_lost(xws))
      return NULL;
   res = CALLOC_STRUCT(virgl_hw_res);
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
      int saved_errno = errno;
      if (virgl_xv6_debug_enabled())
         fprintf(stderr,
                 "virgl-xv6: resource create failed target=%u format=%u bind=0x%x %ux%u size=%u\n",
                 target, format, bind, width, height, size);
      if (saved_errno == EIO)
         virgl_xv6_mark_context_lost(xws, "resource create", saved_errno);
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

static struct virgl_hw_res *
virgl_xv6_resource_create_from_handle(struct virgl_winsys *vws,
                                      struct winsys_handle *whandle,
                                      struct pipe_resource *templ,
                                      uint32_t *plane,
                                      uint32_t *stride,
                                      uint32_t *plane_offset,
                                      uint64_t *modifier,
                                      uint32_t *blob_mem)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct fb_gpu_bo_import_fd import_fd;
   struct fb_gpu_bo_info info;
   struct virgl_hw_res *res;
   static int import_fail_logs;

   if (!whandle || !templ || whandle->type != WINSYS_HANDLE_TYPE_FD ||
       whandle->plane >= 4)
      return NULL;
   if (virgl_xv6_context_lost(xws))
      return NULL;

   memset(&import_fd, 0, sizeof(import_fd));
   import_fd.fd = (int32_t)whandle->handle;
   /*
    * winsys_handle::format is a Gallium pipe_format here, not a DRM fourcc.
    * The xv6 fd exporter has already recorded the dma-buf's DRM metadata in
    * the kernel GEM object, so leave the input metadata empty and let
    * FB_GPU_BO_IMPORT_FD preserve and return the exporter's fourcc/stride.
    */
   import_fd.format = 0;
   import_fd.plane_count = 0;
   if (ioctl(xws->fd, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
       import_fd.handle == 0 || import_fd.addr == 0 ||
       import_fd.size == 0) {
      if (import_fail_logs < 12) {
         fprintf(stderr,
                 "virgl-xv6: import fd failed fd=%u fmt=0x%lx stride=%u off=%u mod=0x%lx errno=%d handle=%u addr=0x%lx size=%lu\n",
                 whandle->handle, (unsigned long)whandle->format,
                 whandle->stride, whandle->offset,
                 (unsigned long)whandle->modifier, errno,
                 import_fd.handle, (unsigned long)import_fd.addr,
                 (unsigned long)import_fd.size);
         import_fail_logs++;
      }
      return NULL;
   }

   memset(&info, 0, sizeof(info));
   info.handle = import_fd.handle;
   if (ioctl(xws->fd, FB_GPU_BO_INFO, &info) < 0 ||
       info.virtio_resource_id == 0) {
      int saved_errno = errno;
      struct fb_gpu_bo_destroy destroy = {
         .handle = import_fd.handle,
      };
      if (import_fail_logs < 12) {
         fprintf(stderr,
                 "virgl-xv6: import fd missing virtio resource fd=%u bo=%u info_res=%u owner=%lu/%d ioctl_errno=%d size=%lu %ux%u stride=%u fmt=0x%x\n",
                 whandle->handle, import_fd.handle, info.virtio_resource_id,
                 (unsigned long)info.virtio_resource_owner_id,
                 info.virtio_resource_owner_tgid, saved_errno,
                 (unsigned long)import_fd.size, import_fd.width,
                 import_fd.height, import_fd.pitch, import_fd.format);
         import_fail_logs++;
      }
      munmap((void *)(uintptr_t)import_fd.addr, (size_t)import_fd.size);
      ioctl(xws->fd, FB_GPU_BO_DESTROY, &destroy);
      return NULL;
   }

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res) {
      struct fb_gpu_bo_destroy destroy = {
         .handle = import_fd.handle,
      };
      munmap((void *)(uintptr_t)import_fd.addr, (size_t)import_fd.size);
      ioctl(xws->fd, FB_GPU_BO_DESTROY, &destroy);
      return NULL;
   }

   res->res_handle = info.virtio_resource_id;
   res->bo_handle = import_fd.handle;
   res->imported = true;
   res->size = import_fd.size;
   res->ptr = (void *)(uintptr_t)import_fd.addr;
   res->bind = templ->bind;
   res->format = templ->format;
   res->width = import_fd.width ? import_fd.width : templ->width0;
   res->height = import_fd.height ? import_fd.height : templ->height0;
   res->depth = templ->depth0 ? templ->depth0 : 1;
   res->last_level = templ->last_level;
   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->num_cs_references, 0);

   if (xws->ctx_id) {
      struct fb_gpu_virgl_resource_attach attach;

      memset(&attach, 0, sizeof(attach));
      attach.ctx_id = xws->ctx_id;
      attach.resource_id = res->res_handle;
      attach.handle = res->bo_handle;
      if (ioctl(xws->fd, FB_GPU_VIRGL_RESOURCE_ATTACH, &attach) < 0) {
         int saved_errno = errno;
         static int attach_fail_logs;
         struct fb_gpu_bo_destroy destroy = {
            .handle = import_fd.handle,
         };

         if (attach_fail_logs < 12) {
            fprintf(stderr,
                    "virgl-xv6: import attach failed ctx=%u resource=%u bo=%u errno=%d\n",
                    xws->ctx_id, res->res_handle, res->bo_handle,
                    saved_errno);
            attach_fail_logs++;
         }
         if (saved_errno == EIO)
            virgl_xv6_mark_context_lost(xws, "resource attach",
                                        saved_errno);
         munmap((void *)(uintptr_t)import_fd.addr, (size_t)import_fd.size);
         ioctl(xws->fd, FB_GPU_BO_DESTROY, &destroy);
         FREE(res);
         return NULL;
      }
   }

   *plane = whandle->plane;
   *stride = import_fd.pitch ? import_fd.pitch : whandle->stride;
   *plane_offset = whandle->offset;
   *modifier = import_fd.modifier;
   *blob_mem = 0;

   if (virgl_xv6_debug_enabled())
      fprintf(stderr,
              "virgl-xv6: import fd=%u bo=%u resource=%u %ux%u stride=%u size=%lu\n",
              whandle->handle, res->bo_handle, res->res_handle,
              res->width, res->height, *stride, (unsigned long)res->size);
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
   int64_t t0 = virgl_xv6_perf_enabled() ? virgl_xv6_now_us() : 0;
   int ret;

   if (virgl_xv6_context_lost(xws))
      return -EIO;

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

   ret = ioctl(xws->fd, from_host ? FB_GPU_VIRGL_TRANSFER_FROM_HOST :
                                    FB_GPU_VIRGL_TRANSFER_TO_HOST,
               &transfer);
   if (ret != 0 && errno == EIO)
      virgl_xv6_mark_context_lost(xws, from_host ? "transfer from host" :
                                  "transfer to host", errno);
   if (t0 > 0)
      virgl_xv6_perf_note_transfer(virgl_xv6_now_us() - t0, from_host,
                                   ret, box->width, box->height);
   return ret;
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
virgl_xv6_fence_create(uint64_t fence_id, int fd, bool external)
{
   struct virgl_xv6_fence *fence = CALLOC_STRUCT(virgl_xv6_fence);
   if (!fence)
      return NULL;

   pipe_reference_init(&fence->reference, 1);
   fence->fd = fd;
   fence->external = external;
   fence->fence_id = fence_id;
   return (struct pipe_fence_handle *)fence;
}

static int
virgl_xv6_export_fence_fd(struct virgl_xv6_winsys *xws, uint64_t fence_id)
{
   struct fb_gpu_virgl_fence_export_fd req;

   if (fence_id == 0)
      return -1;

   memset(&req, 0, sizeof(req));
   req.fd = -1;
   req.fence = fence_id;
   if (ioctl(xws->fd, FB_GPU_VIRGL_FENCE_EXPORT_FD, &req) < 0 ||
       req.fd < 0)
      return -1;
   return req.fd;
}

static int
virgl_xv6_submit_cmd(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                     struct pipe_fence_handle **fence)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct virgl_xv6_cmd_buf *cbuf = virgl_xv6_cmd_buf(_cbuf);
   struct fb_gpu_virgl_submit submit;
   uint32_t *resource_ids = NULL;
   uint32_t resource_count = 0;
   int64_t t0 = virgl_xv6_perf_enabled() ? virgl_xv6_now_us() : 0;
   int ret;

   if (cbuf->base.cdw == 0)
      return 0;

   if (virgl_xv6_context_lost(xws)) {
      virgl_xv6_release_all_res(xws, cbuf);
      cbuf->base.cdw = 0;
      return -EIO;
   }

   if (cbuf->cres != 0) {
      resource_ids = CALLOC(cbuf->cres, sizeof(*resource_ids));
      if (resource_ids) {
         for (unsigned i = 0; i < cbuf->cres; i++) {
            if (cbuf->res_bo[i])
               resource_ids[resource_count++] = cbuf->res_bo[i]->res_handle;
         }
      }
   }

   memset(&submit, 0, sizeof(submit));
   submit.ctx_id = xws->ctx_id;
   submit.flags = xws->sync_submit ? 0 : FB_GPU_VIRGL_SUBMIT_ASYNC;
   if (virgl_xv6_should_force_context_loss(xws))
      submit.flags |= FB_GPU_VIRGL_SUBMIT_FORCE_FAIL;
   submit.cmd = (uint64_t)(uintptr_t)cbuf->base.buf;
   submit.cmd_size = cbuf->base.cdw * sizeof(uint32_t);
   if (resource_ids && resource_count != 0) {
      submit.resource_count = resource_count;
      submit.resources = (uint64_t)(uintptr_t)resource_ids;
   }

   ret = ioctl(xws->fd, FB_GPU_VIRGL_SUBMIT, &submit);
   if (ret != 0)
      virgl_xv6_mark_context_lost(xws, "submit", errno);
   if (t0 > 0)
      virgl_xv6_perf_note_submit(virgl_xv6_now_us() - t0,
                                 submit.cmd_size, resource_count, ret);
   FREE(resource_ids);
   if (virgl_xv6_debug_enabled())
      fprintf(stderr, "virgl-xv6: submit cdw=%u bytes=%u sync=%d ret=%d fence=%lu signaled=%lu\n",
              cbuf->base.cdw, submit.cmd_size, xws->sync_submit ? 1 : 0,
              ret, (unsigned long)submit.fence,
              (unsigned long)submit.signaled);
   if (fence && ret == 0) {
      int fd = vws->supports_fences ?
         virgl_xv6_export_fence_fd(xws, submit.fence) : -1;
      *fence = virgl_xv6_fence_create(submit.fence, fd, false);
   }

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
   /*
    * xv6 does not enable virgl encoded transfers. Advertising any copy-transfer
    * capability lets Mesa allocate texture resources through the staging path;
    * the staging manager is initialized only when encoded transfers are enabled.
    * Keep regular virgl transfers enabled, but hide copy-transfer staging.
    */
   caps->caps.v2.capability_bits &= ~VIRGL_CAP_COPY_TRANSFER;
   caps->caps.v2.capability_bits_v2 &=
      ~VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS;
   if (virgl_xv6_debug_enabled())
      fprintf(stderr, "virgl-xv6: caps ret=%d id=%u version=%u size=%u\n",
              ret, req.capset_id, req.capset_version, req.size);
   return ret;
}

static struct pipe_fence_handle *
virgl_xv6_cs_create_fence(struct virgl_winsys *vws, int fd)
{
   (void)vws;
   if (!vws->supports_fences || fd < 0)
      return NULL;
   fd = virgl_xv6_dupfd_cloexec(fd);
   if (fd < 0)
      return NULL;
   return virgl_xv6_fence_create(0, fd, true);
}

static bool
virgl_xv6_fence_wait(struct virgl_winsys *vws,
                     struct pipe_fence_handle *fence,
                     uint64_t timeout)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct virgl_xv6_fence *xfence = virgl_xv6_fence(fence);
   struct fb_gpu_virgl_fence req;
   struct fb_gpu_virgl_fence_query_fd fd_req;
   int64_t t0 = virgl_xv6_perf_enabled() ? virgl_xv6_now_us() : 0;
   bool ok;

   (void)timeout;
   if (virgl_xv6_context_lost(xws))
      return false;
   if (xfence->fd >= 0) {
      if (!virgl_xv6_wait_fd(xfence->fd, timeout)) {
         if (t0 > 0)
            virgl_xv6_perf_note_wait(virgl_xv6_now_us() - t0, true,
                                     false, timeout);
         return false;
      }
      if (xfence->fence_id == 0) {
         if (t0 > 0)
            virgl_xv6_perf_note_wait(virgl_xv6_now_us() - t0, true,
                                     true, timeout);
         return true;
      }
      memset(&fd_req, 0, sizeof(fd_req));
      fd_req.fd = xfence->fd;
      fd_req.flags = FB_GPU_VIRGL_FENCE_WAIT;
      ok = ioctl(xws->fd, FB_GPU_VIRGL_FENCE_QUERY_FD, &fd_req) == 0 &&
           fd_req.signaled >= xfence->fence_id;
      if (t0 > 0)
         virgl_xv6_perf_note_wait(virgl_xv6_now_us() - t0, true, ok,
                                  timeout);
      return ok;
   }

   if (xfence->fence_id == 0) {
      if (t0 > 0)
         virgl_xv6_perf_note_wait(virgl_xv6_now_us() - t0, false, true,
                                  timeout);
      return true;
   }

   memset(&req, 0, sizeof(req));
   req.flags = FB_GPU_VIRGL_FENCE_WAIT;
   req.wait_for = xfence->fence_id;
   ok = ioctl(xws->fd, FB_GPU_VIRGL_FENCE, &req) == 0 &&
        req.signaled >= xfence->fence_id;
   if (!ok && errno == EIO)
      virgl_xv6_mark_context_lost(xws, "fence wait", errno);
   if (t0 > 0)
      virgl_xv6_perf_note_wait(virgl_xv6_now_us() - t0, false, ok,
                               timeout);
   return ok;
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
                      newf ? &newf->reference : NULL)) {
      if (old && old->fd >= 0)
         close(old->fd);
      FREE(old);
   }
   *dst = src;
}

static void
virgl_xv6_fence_server_sync(struct virgl_winsys *vws,
                            struct virgl_cmd_buf *cbuf,
                            struct pipe_fence_handle *fence)
{
   struct virgl_xv6_fence *xfence = virgl_xv6_fence(fence);

   (void)vws;
   (void)cbuf;
   if (!xfence || !xfence->external || xfence->fd < 0)
      return;

   (void)virgl_xv6_wait_fd(xfence->fd, OS_TIMEOUT_INFINITE);
}

static int
virgl_xv6_fence_get_fd(struct virgl_winsys *vws,
                       struct pipe_fence_handle *fence)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct virgl_xv6_fence *xfence = virgl_xv6_fence(fence);

   if (!vws->supports_fences || !xfence)
      return -1;
   if (xfence->fd >= 0)
      return virgl_xv6_dupfd_cloexec(xfence->fd);
   return virgl_xv6_export_fence_fd(xws, xfence->fence_id);
}

static uint32_t
virgl_xv6_resource_get_storage_size(struct virgl_winsys *vws,
                                    struct virgl_hw_res *res)
{
   (void)vws;
   return res->size;
}

static bool
virgl_xv6_resource_ensure_kms_bo(struct virgl_xv6_winsys *xws,
                                 struct virgl_hw_res *res)
{
   struct fb_gpu_virgl_resource_export_fd export_fd;
   struct fb_gpu_bo_import_fd import_fd;

   if (!res)
      return false;
   if (res->bo_handle != 0)
      return true;

   memset(&export_fd, 0, sizeof(export_fd));
   export_fd.resource_id = res->res_handle;
   export_fd.fd = -1;
   if (ioctl(xws->fd, FB_GPU_VIRGL_RESOURCE_EXPORT_FD, &export_fd) < 0 ||
       export_fd.fd < 0)
      return false;

   memset(&import_fd, 0, sizeof(import_fd));
   import_fd.fd = export_fd.fd;
   if (ioctl(xws->fd, FB_GPU_BO_IMPORT_FD, &import_fd) < 0 ||
       import_fd.handle == 0 || import_fd.addr == 0 ||
       import_fd.size == 0) {
      close(export_fd.fd);
      return false;
   }
   close(export_fd.fd);

   res->bo_handle = import_fd.handle;
   res->kms_ptr = (void *)(uintptr_t)import_fd.addr;
   res->kms_size = import_fd.size;
   if (virgl_xv6_debug_enabled())
      fprintf(stderr,
              "virgl-xv6: kms handle resource=%u bo=%u %ux%u stride=%u size=%lu\n",
              res->res_handle, res->bo_handle, import_fd.width,
              import_fd.height, import_fd.pitch,
              (unsigned long)import_fd.size);
   return true;
}

static bool
virgl_xv6_resource_get_handle(struct virgl_winsys *vws,
                              struct virgl_hw_res *res, uint32_t stride,
                              struct winsys_handle *whandle)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);
   struct fb_gpu_virgl_resource_export_fd export_fd;

   if (!res || !whandle)
      return false;
   if (virgl_xv6_context_lost(xws))
      return false;

   if (whandle->type == WINSYS_HANDLE_TYPE_KMS ||
       whandle->type == WINSYS_HANDLE_TYPE_SHARED) {
      if (!virgl_xv6_resource_ensure_kms_bo(xws, res))
         return false;
      whandle->handle = res->bo_handle;
   } else if (whandle->type == WINSYS_HANDLE_TYPE_FD) {
      memset(&export_fd, 0, sizeof(export_fd));
      export_fd.resource_id = res->res_handle;
      if (ioctl(xws->fd, FB_GPU_VIRGL_RESOURCE_EXPORT_FD, &export_fd) < 0 ||
          export_fd.fd < 0)
         return false;
      whandle->handle = (unsigned)export_fd.fd;
      whandle->size = export_fd.size;
      if (export_fd.pitch)
         stride = export_fd.pitch;
   } else {
      return false;
   }

   whandle->stride = stride;
   whandle->offset = 0;
   whandle->modifier = 0;
   if (virgl_xv6_debug_enabled())
      fprintf(stderr,
              "virgl-xv6: export resource id=%u type=%u handle=%u stride=%u size=%lu\n",
              res->res_handle, whandle->type, whandle->handle, stride,
              (unsigned long)whandle->size);
   return true;
}

static enum pipe_reset_status
virgl_xv6_get_context_reset_status(struct virgl_winsys *vws)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);

   return virgl_xv6_context_lost(xws) ? PIPE_GUILTY_CONTEXT_RESET :
                                        PIPE_NO_RESET;
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

static int
virgl_xv6_get_fd(struct virgl_winsys *vws)
{
   struct virgl_xv6_winsys *xws = virgl_xv6_winsys(vws);

   return xws->fd;
}

struct virgl_winsys *
virgl_xv6_winsys_create_for_fd(int fd)
{
   struct virgl_xv6_winsys *xws = CALLOC_STRUCT(virgl_xv6_winsys);
   struct fb_gpu_virgl_ctx ctx;
   struct virgl_drm_caps caps;
   const char *device_path;

   if (!xws)
      return NULL;

   xws->fd = -1;
   if (fd >= 0)
      xws->fd = dup(fd);
   device_path = getenv("XV6_VIRGL_DEVICE");
   if (xws->fd < 0 && device_path && device_path[0])
      xws->fd = open(device_path, O_RDWR | O_CLOEXEC);
   if (xws->fd < 0)
      xws->fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (xws->fd < 0)
      xws->fd = open("/dev/gpu0", O_RDWR | O_CLOEXEC);
   if (xws->fd < 0)
      xws->fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
   if (xws->fd < 0)
      goto fail;

   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.debug_name, sizeof(ctx.debug_name), "mesa-virgl");
   if (ioctl(xws->fd, FB_GPU_VIRGL_CTX_CREATE, &ctx) < 0 || ctx.ctx_id == 0)
      goto fail;
   xws->ctx_id = ctx.ctx_id;
   xws->sync_submit = virgl_xv6_env_enabled("XV6_VIRGL_SYNC_SUBMIT");
   if (xws->sync_submit)
      fprintf(stderr, "virgl-xv6: context %u using synchronous submit\n",
              xws->ctx_id);
   xws->force_loss_after_seconds =
      virgl_xv6_env_u64("XV6_VIRGL_FORCE_CONTEXT_LOSS_AFTER_SECONDS");
   xws->force_loss_start_nsec = virgl_xv6_now_nsec();
   if (xws->force_loss_after_seconds != 0)
      fprintf(stderr,
              "virgl-xv6: context %u will force loss after %lu seconds\n",
              xws->ctx_id, (unsigned long)xws->force_loss_after_seconds);

   (void)mtx_init(&xws->mutex, mtx_plain);

   xws->base.destroy = virgl_xv6_destroy;
   xws->base.get_fd = virgl_xv6_get_fd;
   xws->base.transfer_put = virgl_xv6_transfer_put;
   xws->base.transfer_get = virgl_xv6_transfer_get;
   xws->base.resource_create = virgl_xv6_resource_create;
   xws->base.resource_reference = virgl_xv6_resource_reference;
   xws->base.resource_map = virgl_xv6_resource_map;
   xws->base.resource_wait = virgl_xv6_resource_wait;
   xws->base.resource_is_busy = virgl_xv6_resource_is_busy;
   xws->base.resource_create_from_handle =
      virgl_xv6_resource_create_from_handle;
   xws->base.resource_set_type = virgl_xv6_resource_set_type;
   xws->base.resource_get_handle = virgl_xv6_resource_get_handle;
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
   xws->base.fence_server_sync = virgl_xv6_fence_server_sync;
   xws->base.fence_get_fd = virgl_xv6_fence_get_fd;
   /*
    * Keep context loss fail-closed inside the winsys.  The staged WebKitGTK
    * runtime crashes in its Skia worker cleanup when Mesa advertises graphics
    * reset status for this path, so do not expose the query hook here.
    */
   xws->base.supports_fences = virgl_xv6_native_fences_enabled();
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

struct virgl_winsys *
virgl_xv6_winsys_create(void)
{
   return virgl_xv6_winsys_create_for_fd(-1);
}
