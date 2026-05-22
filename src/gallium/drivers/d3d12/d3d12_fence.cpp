/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "d3d12_fence.h"

#include "d3d12_context.h"
#include "d3d12_screen.h"

#include "util/u_memory.h"

#include <dxguids/dxguids.h>

static void
destroy_fence(struct d3d12_fence *fence)
{
   if (fence->type == PIPE_FD_TYPE_NATIVE_SYNC)
      d3d12_fence_close_event(fence->event, fence->event_fd);
   else
      fence->cmdqueue_fence->Release();
   FREE(fence);
}

struct d3d12_fence *
d3d12_create_fence(struct d3d12_screen *screen, bool signal_new)
{
   struct d3d12_fence *ret =
      d3d12_create_fence_raw(screen->fence, screen->fence_value + (signal_new ? 1 : 0));
   if (!ret) {
      debug_printf("CALLOC_STRUCT failed\n");
      return NULL;
   }

   if (signal_new) {
      ++screen->fence_value;
      if (FAILED(screen->cmdqueue->Signal(screen->fence, ret->value)))
         goto fail;
   }
   return ret;

fail:
   destroy_fence(ret);
   return NULL;
}

struct d3d12_fence *
d3d12_create_fence_raw(ID3D12Fence *fence, uint64_t value)
{
   struct d3d12_fence *ret = CALLOC_STRUCT(d3d12_fence);
   if (!ret) {
      debug_printf("CALLOC_STRUCT failed\n");
      return NULL;
   }

   ret->type = PIPE_FD_TYPE_NATIVE_SYNC;
   ret->cmdqueue_fence = fence;
   ret->value = value;
   ret->event = 0;
   ret->event_fd = -1;
   ret->signaled = false;

   pipe_reference_init(&ret->reference, 1);
   return ret;
}

struct d3d12_fence *
d3d12_open_fence(struct d3d12_screen *screen, HANDLE handle, const void *name, pipe_fd_type type)
{
   struct d3d12_fence *ret = CALLOC_STRUCT(d3d12_fence);
   if (!ret) {
      debug_printf("CALLOC_STRUCT failed\n");
      return NULL;
   }

   HANDLE handle_to_close = nullptr;
   assert(!!handle ^ !!name);
   if (name) {
      screen->dev->OpenSharedHandleByName((LPCWSTR)name, GENERIC_ALL, &handle_to_close);
      handle = handle_to_close;
   }

   screen->dev->OpenSharedHandle(handle, IID_PPV_ARGS(&ret->cmdqueue_fence));
   if (!ret->cmdqueue_fence) {
      free(ret);
      return NULL;
   }

   ret->type = type;
   pipe_reference_init(&ret->reference, 1);
   return ret;
}

void
d3d12_fence_reference(struct d3d12_fence **ptr, struct d3d12_fence *fence)
{
   if (pipe_reference(&(*ptr)->reference, &fence->reference))
      destroy_fence((struct d3d12_fence *)*ptr);

   *ptr = fence;
}

void
d3d12_video_destroy_fence(struct pipe_video_codec *codec, struct pipe_fence_handle *fence)
{
   d3d12_fence_reference((struct d3d12_fence **)&fence, nullptr);
}

static void
fence_reference(struct pipe_screen *pscreen,
                struct pipe_fence_handle **pptr,
                struct pipe_fence_handle *pfence)
{
   d3d12_fence_reference((struct d3d12_fence **)pptr, d3d12_fence(pfence));
}

static bool
d3d12_fence_completed(ID3D12Fence *cmdqueue_fence, uint64_t target,
                      bool *device_removed)
{
   uint64_t completed = cmdqueue_fence->GetCompletedValue();

   *device_removed = false;
   if (completed == UINT64_MAX) {
      debug_printf("D3D12: fence completed value is UINT64_MAX; treating fence "
                   "target %llu as not signaled (possible device removal)\n",
                   (unsigned long long)target);
      *device_removed = true;
      return false;
   }

   return completed >= target;
}

static bool
d3d12_fence_device_removed_after_event(ID3D12Fence *cmdqueue_fence,
                                       uint64_t target)
{
   uint64_t completed = cmdqueue_fence->GetCompletedValue();

   if (completed == UINT64_MAX) {
      debug_printf("D3D12: fence completed value is UINT64_MAX after event; "
                   "treating fence target %llu as not signaled "
                   "(possible device removal)\n",
                   (unsigned long long)target);
      return true;
   }

   if (completed < target) {
      static unsigned log_count;

      if (log_count++ < 8) {
         debug_printf("D3D12: fence event signaled before completed value "
                      "caught up: completed=%llu target=%llu\n",
                      (unsigned long long)completed,
                      (unsigned long long)target);
      }
   }

   return false;
}

bool
d3d12_fence_finish(struct d3d12_fence *fence, uint64_t timeout_ns)
{
   if (!fence || fence->type != PIPE_FD_TYPE_NATIVE_SYNC ||
       !fence->cmdqueue_fence)
      return false;
   if (fence->signaled)
      return true;

   bool device_removed = false;
   bool complete = d3d12_fence_completed(fence->cmdqueue_fence, fence->value,
                                         &device_removed);
   if (device_removed)
      return false;
   if (!complete && timeout_ns) {
      if (!fence->event) {
         fence->event = d3d12_fence_create_event(&fence->event_fd);
         if (!fence->event)
            return false;
      }
      if (FAILED(fence->cmdqueue_fence->SetEventOnCompletion(fence->value,
                                                              fence->event)))
         return false;
      if (d3d12_fence_wait_event(fence->event, fence->event_fd, timeout_ns)) {
         if (d3d12_fence_device_removed_after_event(fence->cmdqueue_fence,
                                                    fence->value))
            return false;
         complete = true;
      } else {
         complete = d3d12_fence_completed(fence->cmdqueue_fence, fence->value,
                                          &device_removed);
         if (device_removed)
            return false;
         if (complete) {
            static unsigned log_count;

            if (log_count++ < 8) {
               debug_printf("D3D12: fence event wait returned unsignaled, "
                            "but completed value reached target %llu\n",
                            (unsigned long long)fence->value);
            }
         }
      }
   }

   fence->signaled = complete;
   return complete;
}

static bool
fence_finish(struct pipe_screen *pscreen, struct pipe_context *pctx,
             struct pipe_fence_handle *pfence, uint64_t timeout_ns)
{
   bool ret = d3d12_fence_finish(d3d12_fence(pfence), timeout_ns);
   if (ret && pctx) {
      pctx = threaded_context_unwrap_sync(pctx);
      struct d3d12_context *ctx = d3d12_context(pctx);
      d3d12_foreach_submitted_batch(ctx, batch)
         d3d12_reset_batch(ctx, batch, 0);
   }
   return ret;
}

void
d3d12_fence_signal_impl(struct d3d12_fence *fence, ID3D12CommandQueue *queue, uint64_t value)
{
   if (fence->type == PIPE_FD_TYPE_NATIVE_SYNC)
      value = fence->value;
   queue->Signal(fence->cmdqueue_fence, value);
}

void
d3d12_fence_wait_impl(struct d3d12_fence *fence, ID3D12CommandQueue *queue, uint64_t value)
{
   if (fence->type == PIPE_FD_TYPE_NATIVE_SYNC)
      value = fence->value;
   queue->Wait(fence->cmdqueue_fence, value);
}

void
d3d12_screen_fence_init(struct pipe_screen *pscreen)
{
   pscreen->fence_reference = fence_reference;
   pscreen->fence_finish = fence_finish;
}
