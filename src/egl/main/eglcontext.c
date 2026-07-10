/**************************************************************************
 *
 * Copyright 2008 VMware, Inc.
 * Copyright 2009-2010 Chia-I Wu <olvaffe@gmail.com>
 * Copyright 2010-2011 LunarG, Inc.
 * All Rights Reserved.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "eglcontext.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/macros.h"
#include "util/u_debug.h"
#include "eglconfig.h"
#include "eglcurrent.h"
#include "egldisplay.h"
#include "egllog.h"
#include "eglsurface.h"

enum { XV6_EGL_CONTEXT_FAILURE_ROWS_PER_PID = 16 };

static simple_mtx_t xv6_egl_context_trace_lock = SIMPLE_MTX_INITIALIZER;
static pid_t xv6_egl_context_trace_pid = -1;
static unsigned int xv6_egl_context_trace_rows;

static bool
xv6_egl_context_trace_enabled(void)
{
   return debug_get_bool_option("XV6_MESA_EGL_CONTEXT_TRACE", false);
}

static bool
xv6_egl_context_trace_reserve(pid_t pid, unsigned int *row)
{
   bool allowed;

   simple_mtx_lock(&xv6_egl_context_trace_lock);
   if (xv6_egl_context_trace_pid != pid) {
      xv6_egl_context_trace_pid = pid;
      xv6_egl_context_trace_rows = 0;
   }
   allowed = xv6_egl_context_trace_rows <
             XV6_EGL_CONTEXT_FAILURE_ROWS_PER_PID;
   if (allowed) {
      xv6_egl_context_trace_rows++;
      *row = xv6_egl_context_trace_rows;
   }
   simple_mtx_unlock(&xv6_egl_context_trace_lock);
   return allowed;
}

static const char *
xv6_egl_context_api_name(EGLenum api)
{
   switch (api) {
   case EGL_OPENGL_API:
      return "opengl";
   case EGL_OPENGL_ES_API:
      return "opengles";
   case EGL_OPENVG_API:
      return "openvg";
   case EGL_NONE:
      return "none";
   default:
      return "unknown";
   }
}

static const char *
xv6_egl_context_platform_name(_EGLPlatformType platform)
{
   switch (platform) {
   case _EGL_PLATFORM_X11:
      return "x11";
   case _EGL_PLATFORM_XCB:
      return "xcb";
   case _EGL_PLATFORM_WAYLAND:
      return "wayland";
   case _EGL_PLATFORM_DRM:
      return "drm";
   case _EGL_PLATFORM_ANDROID:
      return "android";
   case _EGL_PLATFORM_HAIKU:
      return "haiku";
   case _EGL_PLATFORM_SURFACELESS:
      return "surfaceless";
   case _EGL_PLATFORM_DEVICE:
      return "device";
   case _EGL_PLATFORM_WINDOWS:
      return "windows";
   case _EGL_INVALID_PLATFORM:
      return "invalid";
   default:
      return "unknown";
   }
}

static const char *
xv6_egl_context_error_name(EGLint err)
{
   switch (err) {
   case EGL_SUCCESS:
      return "EGL_SUCCESS";
   case EGL_BAD_ACCESS:
      return "EGL_BAD_ACCESS";
   case EGL_BAD_ALLOC:
      return "EGL_BAD_ALLOC";
   case EGL_BAD_ATTRIBUTE:
      return "EGL_BAD_ATTRIBUTE";
   case EGL_BAD_CONFIG:
      return "EGL_BAD_CONFIG";
   case EGL_BAD_CONTEXT:
      return "EGL_BAD_CONTEXT";
   case EGL_BAD_CURRENT_SURFACE:
      return "EGL_BAD_CURRENT_SURFACE";
   case EGL_BAD_DISPLAY:
      return "EGL_BAD_DISPLAY";
   case EGL_BAD_MATCH:
      return "EGL_BAD_MATCH";
   case EGL_BAD_NATIVE_PIXMAP:
      return "EGL_BAD_NATIVE_PIXMAP";
   case EGL_BAD_NATIVE_WINDOW:
      return "EGL_BAD_NATIVE_WINDOW";
   case EGL_BAD_PARAMETER:
      return "EGL_BAD_PARAMETER";
   case EGL_BAD_SURFACE:
      return "EGL_BAD_SURFACE";
   default:
      return "EGL_ERROR_UNKNOWN";
   }
}

static void
xv6_egl_context_format_attribs(char *buf, size_t size,
                               const EGLint *attrib_list)
{
   size_t off = 0;

   if (size == 0)
      return;
   buf[0] = '\0';
   if (!attrib_list) {
      snprintf(buf, size, "NULL");
      return;
   }
   for (unsigned int i = 0; i < 128; i += 2) {
      EGLint attr = attrib_list[i];
      int n;

      if (attr == EGL_NONE) {
         snprintf(buf + off, off < size ? size - off : 0,
                  "%sEGL_NONE(0x%x)", off ? "," : "", EGL_NONE);
         return;
      }
      if (off >= size)
         return;
      n = snprintf(buf + off, size - off, "%s0x%x=0x%x",
                   off ? "," : "", (unsigned int)attr,
                   (unsigned int)attrib_list[i + 1]);
      if (n < 0)
         return;
      if ((size_t)n >= size - off) {
         buf[size - 1] = '\0';
         return;
      }
      off += (size_t)n;
   }
   if (off < size)
      snprintf(buf + off, size - off, "%s...", off ? "," : "...");
}

static void
xv6_egl_context_trace_failure(const _EGLContext *ctx,
                               const _EGLDisplay *disp,
                               const _EGLContext *share_list,
                               const EGLint *attrib_list, EGLint err,
                               EGLint first_rejected_attr,
                               const char *detail)
{
   char attrs[4096];
   unsigned int row = 0;
   pid_t pid;

   if (!xv6_egl_context_trace_enabled())
      return;
   pid = getpid();
   if (!xv6_egl_context_trace_reserve(pid, &row))
      return;
   xv6_egl_context_format_attribs(attrs, sizeof(attrs), attrib_list);
   fprintf(stderr,
           "xv6_mesa_egl_context_trace phase=context_failure pid=%ld "
           "row=%u cap=%u bound_api=0x%x bound_api_name=%s "
           "display=%p display_platform=%d display_platform_name=%s "
           "display_platform_handle=%p display_version=%d "
           "display_initialized=%d display_client_apis=0x%x "
           "gate_khr_create_context=%d gate_khr_create_context_no_error=%d "
           "gate_khr_no_config_context=%d gate_khr_surfaceless_context=%d "
           "gate_ext_create_context_robustness=%d "
           "gate_angle_webgl_compatibility=%d robust_buffer_access=%d "
           "config=%p config_renderable_type=0x%x share=%p "
           "share_no_error=%d share_reset=0x%x final_version=%d.%d "
           "final_flags=0x%x final_profile=0x%x final_reset=0x%x "
           "final_priority=0x%x final_release=0x%x final_no_error=%d "
           "final_webgl=%d final_protected=%d error=0x%x error_name=%s "
           "first_rejected_attr=0x%x detail=%s attribs=\"%s\"\n",
           (long)pid, row, XV6_EGL_CONTEXT_FAILURE_ROWS_PER_PID,
           (unsigned int)ctx->ClientAPI,
           xv6_egl_context_api_name(ctx->ClientAPI), (const void *)disp,
           disp ? (int)disp->Platform : (int)_EGL_INVALID_PLATFORM,
           disp ? xv6_egl_context_platform_name(disp->Platform) : "missing",
           disp ? disp->PlatformDisplay : NULL, disp ? disp->Version : -1,
           disp ? (disp->Initialized ? 1 : 0) : -1,
           disp ? disp->ClientAPIs : -1,
           disp ? (disp->Extensions.KHR_create_context ? 1 : 0) : -1,
           disp ? (disp->Extensions.KHR_create_context_no_error ? 1 : 0) : -1,
           disp ? (disp->Extensions.KHR_no_config_context ? 1 : 0) : -1,
           disp ? (disp->Extensions.KHR_surfaceless_context ? 1 : 0) : -1,
           disp ? (disp->Extensions.EXT_create_context_robustness ? 1 : 0) : -1,
           disp ? (disp->Extensions.ANGLE_create_context_webgl_compatibility ? 1 : 0) : -1,
           disp ? (disp->RobustBufferAccess ? 1 : 0) : -1,
           (void *)ctx->Config,
           ctx->Config ? (unsigned int)ctx->Config->RenderableType : 0,
           (void *)share_list, share_list ? (share_list->NoError ? 1 : 0) : -1,
           share_list ? (unsigned int)share_list->ResetNotificationStrategy : 0,
           ctx->ClientMajorVersion, ctx->ClientMinorVersion,
           (unsigned int)ctx->Flags, (unsigned int)ctx->Profile,
           (unsigned int)ctx->ResetNotificationStrategy,
           (unsigned int)ctx->ContextPriority,
           (unsigned int)ctx->ReleaseBehavior, ctx->NoError ? 1 : 0,
           ctx->WebGLCompatibility ? 1 : 0, ctx->Protected ? 1 : 0,
           (unsigned int)err, xv6_egl_context_error_name(err),
           (unsigned int)first_rejected_attr,
           detail ? detail : "unspecified", attrs);
   fflush(stderr);
}

/**
 * Return the API bit (one of EGL_xxx_BIT) of the context.
 */
static EGLint
_eglGetContextAPIBit(_EGLContext *ctx)
{
   EGLint bit = 0;

   switch (ctx->ClientAPI) {
   case EGL_OPENGL_ES_API:
      switch (ctx->ClientMajorVersion) {
      case 1:
         bit = EGL_OPENGL_ES_BIT;
         break;
      case 2:
         bit = EGL_OPENGL_ES2_BIT;
         break;
      case 3:
         bit = EGL_OPENGL_ES3_BIT_KHR;
         break;
      default:
         break;
      }
      break;
   case EGL_OPENVG_API:
      bit = EGL_OPENVG_BIT;
      break;
   case EGL_OPENGL_API:
      bit = EGL_OPENGL_BIT;
      break;
   default:
      break;
   }

   return bit;
}

/**
 * Parse the list of context attributes and return the proper error code.
 */
static EGLint
_eglParseContextAttribList(_EGLContext *ctx, _EGLDisplay *disp,
                           const EGLint *attrib_list,
                           EGLint *first_rejected_attr_out,
                           const char **detail_out)
{
   EGLenum api = ctx->ClientAPI;
   EGLint i, err = EGL_SUCCESS;
   EGLint first_rejected_attr = EGL_NONE;
   const char *detail = "success";

   if (!attrib_list) {
      *first_rejected_attr_out = EGL_NONE;
      *detail_out = "no-attrib-list";
      return EGL_SUCCESS;
   }

   if (api == EGL_OPENVG_API && attrib_list[0] != EGL_NONE) {
      _eglLog(_EGL_DEBUG, "bad context attribute 0x%04x", attrib_list[0]);
      *first_rejected_attr_out = attrib_list[0];
      *detail_out = "openvg-attribute";
      return EGL_BAD_ATTRIBUTE;
   }

   for (i = 0; attrib_list[i] != EGL_NONE; i++) {
      EGLint attr = attrib_list[i++];
      EGLint val = attrib_list[i];

      switch (attr) {
      case EGL_CONTEXT_CLIENT_VERSION:
         /* The EGL 1.4 spec says:
          *
          *     "attribute EGL_CONTEXT_CLIENT_VERSION is only valid when the
          *      current rendering API is EGL_OPENGL_ES_API"
          *
          * The EGL_KHR_create_context spec says:
          *
          *     "EGL_CONTEXT_MAJOR_VERSION_KHR           0x3098
          *      (this token is an alias for EGL_CONTEXT_CLIENT_VERSION)"
          *
          *     "The values for attributes EGL_CONTEXT_MAJOR_VERSION_KHR and
          *      EGL_CONTEXT_MINOR_VERSION_KHR specify the requested client API
          *      version. They are only meaningful for OpenGL and OpenGL ES
          *      contexts, and specifying them for other types of contexts will
          *      generate an error."
          */
         if ((api != EGL_OPENGL_ES_API &&
              (!disp->Extensions.KHR_create_context ||
               api != EGL_OPENGL_API))) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         ctx->ClientMajorVersion = val;
         break;

      case EGL_CONTEXT_MINOR_VERSION_KHR:
         /* The EGL_KHR_create_context spec says:
          *
          *     "The values for attributes EGL_CONTEXT_MAJOR_VERSION_KHR and
          *      EGL_CONTEXT_MINOR_VERSION_KHR specify the requested client API
          *      version. They are only meaningful for OpenGL and OpenGL ES
          *      contexts, and specifying them for other types of contexts will
          *      generate an error."
          */
         if (!disp->Extensions.KHR_create_context ||
             (api != EGL_OPENGL_ES_API && api != EGL_OPENGL_API)) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         ctx->ClientMinorVersion = val;
         break;

      case EGL_CONTEXT_FLAGS_KHR:
         if (!disp->Extensions.KHR_create_context) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* The EGL_KHR_create_context spec says:
          *
          *     "If the EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR flag bit is set in
          *     EGL_CONTEXT_FLAGS_KHR, then a <debug context> will be created.
          *     [...]
          *     In some cases a debug context may be identical to a non-debug
          *     context. This bit is supported for OpenGL and OpenGL ES
          *     contexts."
          */
         if ((val & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR) &&
             (api != EGL_OPENGL_API && api != EGL_OPENGL_ES_API)) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* The EGL_KHR_create_context spec says:
          *
          *     "If the EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR flag bit
          *     is set in EGL_CONTEXT_FLAGS_KHR, then a <forward-compatible>
          *     context will be created. Forward-compatible contexts are
          *     defined only for OpenGL versions 3.0 and later. They must not
          *     support functionality marked as <deprecated> by that version of
          *     the API, while a non-forward-compatible context must support
          *     all functionality in that version, deprecated or not. This bit
          *     is supported for OpenGL contexts, and requesting a
          *     forward-compatible context for OpenGL versions less than 3.0
          *     will generate an error."
          *
          * Note: since the forward-compatible flag can be set more than one
          * way, the OpenGL version check is performed once, below.
          */
         if ((val & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR) &&
             api != EGL_OPENGL_API) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         if ((val & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR) &&
             api != EGL_OPENGL_API) {
            /* The EGL_KHR_create_context spec says:
             *
             *   10) Which error should be generated if robust buffer access
             *       or reset notifications are requested under OpenGL ES?
             *
             *       As per Issue 6, this extension does not support creating
             *       robust contexts for OpenGL ES. This is only supported via
             *       the EGL_EXT_create_context_robustness extension.
             *
             *       Attempting to use this extension to create robust OpenGL
             *       ES context will generate an EGL_BAD_ATTRIBUTE error. This
             *       specific error is generated because this extension does
             *       not define the EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR
             *       and EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR
             *       bits for OpenGL ES contexts. Thus, use of these bits fall
             *       under condition described by: "If an attribute is
             *       specified that is not meaningful for the client API
             *       type.." in the above specification.
             *
             * The spec requires that we emit the error even if the display
             * supports EGL_EXT_create_context_robustness. To create a robust
             * GLES context, the *attribute*
             * EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT must be used, not the
             * *flag* EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR.
             */
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* The EGL_KHR_create_context spec says:
          *     "If <config> does not support a client API context compatible
          *     with the requested API major and minor version, context flags,
          *     and context reset notification behavior (for client API types
          *     where these attributes are supported), then an EGL_BAD_MATCH
          *     error is generated."
          */
         if ((val & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR) &&
             !disp->RobustBufferAccess) {
            err = EGL_BAD_MATCH;
            break;
         }

         ctx->Flags |= val;
         break;

      case EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR:
         if (!disp->Extensions.KHR_create_context) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* The EGL_KHR_create_context spec says:
          *
          *     "[EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR] is only meaningful for
          *     OpenGL contexts, and specifying it for other types of
          *     contexts, including OpenGL ES contexts, will generate an
          *     error."
          */
         if (api != EGL_OPENGL_API) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         ctx->Profile = val;
         break;

      case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR:
         /* The EGL_KHR_create_context spec says:
          *
          *     "[EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR] is only
          *     meaningful for OpenGL contexts, and specifying it for other
          *     types of contexts, including OpenGL ES contexts, will generate
          *     an error."
          *
          * EGL 1.5 defines EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY
          * (without a suffix) which has the same value as the KHR token,
          * and specifies that it now works with both GL and ES contexts:
          *
          *    "This attribute is supported only for OpenGL and OpenGL ES
          *     contexts."
          */
         if (!(disp->Extensions.KHR_create_context && api == EGL_OPENGL_API) &&
             disp->Version < 15) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* The EGL 1.5 spec says:
          *     "An EGL_BAD_MATCH error is generated if an OpenGL or OpenGL ES
          *     context is requested with robust buffer access and with a
          *     specified reset notification behavior, and the implementation
          *     does not support that behavior."
          *
          * and the EGL_KHR_create_context spec says:
          *     "If <config> does not support a client API context compatible
          *     with the requested API major and minor version, context flags,
          *     and context reset notification behavior (for client API types
          *     where these attributes are supported), then an EGL_BAD_MATCH
          *     error is generated."
          */
         if (val != EGL_NO_RESET_NOTIFICATION_KHR &&
             !disp->Extensions.EXT_create_context_robustness) {
            err = EGL_BAD_MATCH;
            break;
         }

         ctx->ResetNotificationStrategy = val;
         break;

      case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT:
         /* The EGL_EXT_create_context_robustness spec says:
          *
          *     "[EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT] is only
          *     meaningful for OpenGL ES contexts, and specifying it for other
          *     types of contexts will generate an EGL_BAD_ATTRIBUTE error."
          */
         if (!disp->Extensions.EXT_create_context_robustness ||
             api != EGL_OPENGL_ES_API) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         ctx->ResetNotificationStrategy = val;
         break;

      case EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT:
         if (!disp->Extensions.EXT_create_context_robustness) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* The EGL_EXT_create_context_robustness spec says:
          *
          *     "EGL_BAD_CONFIG is generated if
          *     [EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT] is set to EGL_TRUE and
          *     no GL context supporting the GL_EXT_robustness extension and
          *     robust access as described therein can be created."
          */
         if (val == EGL_TRUE && !disp->RobustBufferAccess) {
            err = EGL_BAD_CONFIG;
            break;
         }

         if (val == EGL_TRUE)
            ctx->Flags |= EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR;
         break;

      case EGL_CONTEXT_OPENGL_ROBUST_ACCESS:
         if (disp->Version < 15) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* The EGL 1.5 spec says:
          *     "An EGL_BAD_MATCH error is generated if an OpenGL or OpenGL ES
          *     context is requested with robust buffer access, and the
          *     implementation does not support the corresponding OpenGL or
          *     OpenGL ES extension".
          */
         if (val == EGL_TRUE && !disp->RobustBufferAccess) {
            err = EGL_BAD_MATCH;
            break;
         }

         if (val == EGL_TRUE)
            ctx->Flags |= EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR;
         break;

      case EGL_CONTEXT_OPENGL_DEBUG:
         if (disp->Version < 15) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         if (val == EGL_TRUE)
            ctx->Flags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
         break;

      case EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE:
         if (disp->Version < 15) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         if (val == EGL_TRUE)
            ctx->Flags |= EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;
         break;

      case EGL_CONTEXT_OPENGL_NO_ERROR_KHR:
         if (disp->Version < 14 ||
             !disp->Extensions.KHR_create_context_no_error) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         /* Canonicalize value to EGL_TRUE/EGL_FALSE definitions */
         ctx->NoError = !!val;
         break;

      case EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE:
         if (!disp->Extensions.ANGLE_create_context_webgl_compatibility ||
             api != EGL_OPENGL_ES_API) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         if (val != EGL_TRUE && val != EGL_FALSE) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }

         ctx->WebGLCompatibility = val == EGL_TRUE;
         break;

      case EGL_CONTEXT_PRIORITY_LEVEL_IMG:
         /* The  EGL_IMG_context_priority spec says:
          *
          * "EGL_CONTEXT_PRIORITY_LEVEL_IMG determines the priority level of
          * the context to be created. This attribute is a hint, as an
          * implementation may not support multiple contexts at some
          * priority levels and system policy may limit access to high
          * priority contexts to appropriate system privilege level. The
          * default value for EGL_CONTEXT_PRIORITY_LEVEL_IMG is
          * EGL_CONTEXT_PRIORITY_MEDIUM_IMG."
          */
         {
            int bit;

            switch (val) {
            case EGL_CONTEXT_PRIORITY_REALTIME_NV:
               if (disp->Extensions.NV_context_priority_realtime)
                  bit = __EGL_CONTEXT_PRIORITY_REALTIME_BIT;
               else
                  bit = -1;
               break;
            case EGL_CONTEXT_PRIORITY_HIGH_IMG:
               bit = __EGL_CONTEXT_PRIORITY_HIGH_BIT;
               break;
            case EGL_CONTEXT_PRIORITY_MEDIUM_IMG:
               bit = __EGL_CONTEXT_PRIORITY_MEDIUM_BIT;
               break;
            case EGL_CONTEXT_PRIORITY_LOW_IMG:
               bit = __EGL_CONTEXT_PRIORITY_LOW_BIT;
               break;
            default:
               bit = -1;
               break;
            }

            if (bit < 0) {
               err = EGL_BAD_ATTRIBUTE;
               break;
            }

            /* "This extension allows an EGLContext to be created with a
             * priority hint. It is possible that an implementation will not
             * honour the hint, especially if there are constraints on the
             * number of high priority contexts available in the system, or
             * system policy limits access to high priority contexts to
             * appropriate system privilege level. A query is provided to find
             * the real priority level assigned to the context after creation."
             *
             * We currently assume that the driver applies the priority hint
             * and filters out any it cannot handle during the screen setup,
             * e.g. dri2_setup_screen(). As such we can mask any change that
             * the driver would fail, and ctx->ContextPriority matches the
             * hint applied to the driver/hardware backend.
             */
            if (disp->Extensions.IMG_context_priority & (1 << bit))
               ctx->ContextPriority = val;

            break;
         }

      case EGL_CONTEXT_RELEASE_BEHAVIOR_KHR:
         if (val == EGL_CONTEXT_RELEASE_BEHAVIOR_NONE_KHR ||
             val == EGL_CONTEXT_RELEASE_BEHAVIOR_FLUSH_KHR) {
            ctx->ReleaseBehavior = val;
         } else {
            err = EGL_BAD_ATTRIBUTE;
         }
         break;

      case EGL_PROTECTED_CONTENT_EXT:
         if (!disp->Extensions.EXT_protected_content) {
            err = EGL_BAD_ATTRIBUTE;
            break;
         }
         ctx->Protected = val == EGL_TRUE;
         break;

      default:
         err = EGL_BAD_ATTRIBUTE;
         break;
      }

      if (err != EGL_SUCCESS) {
         _eglLog(_EGL_DEBUG, "bad context attribute 0x%04x", attr);
         if (first_rejected_attr == EGL_NONE) {
            first_rejected_attr = attr;
            detail = "attribute-rejected";
         }
         break;
      }
   }

   if (api == EGL_OPENGL_API) {
      /* The EGL_KHR_create_context spec says:
       *
       *     "If the requested OpenGL version is less than 3.2,
       *     EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR is ignored and the
       *     functionality of the context is determined solely by the
       *     requested version."
       *
       * Since the value is ignored, only validate the setting if the version
       * is >= 3.2.
       */
      if (ctx->ClientMajorVersion >= 4 ||
          (ctx->ClientMajorVersion == 3 && ctx->ClientMinorVersion >= 2)) {
         switch (ctx->Profile) {
         case EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR:
         case EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR:
            break;

         default:
            /* The EGL_KHR_create_context spec says:
             *
             *     "* If an OpenGL context is requested, the requested version
             *        is greater than 3.2, and the value for attribute
             *        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR has no bits set; has
             *        any bits set other than
             * EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR and
             * EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR; has more than
             * one of these bits set; or if the implementation does not support
             * the requested profile, then an EGL_BAD_MATCH error is generated."
             */
            err = EGL_BAD_MATCH;
            break;
         }
      }

      /* The EGL_KHR_create_context spec says:
       *
       *     "* If an OpenGL context is requested and the values for
       *        attributes EGL_CONTEXT_MAJOR_VERSION_KHR and
       *        EGL_CONTEXT_MINOR_VERSION_KHR, when considered together with
       *        the value for attribute
       *        EGL_CONTEXT_FORWARD_COMPATIBLE_BIT_KHR, specify an OpenGL
       *        version and feature set that are not defined, than an
       *        EGL_BAD_MATCH error is generated.
       *
       *        ... Thus, examples of invalid combinations of attributes
       *        include:
       *
       *          - Major version < 1 or > 4
       *          - Major version == 1 and minor version < 0 or > 5
       *          - Major version == 2 and minor version < 0 or > 1
       *          - Major version == 3 and minor version < 0 or > 2
       *          - Major version == 4 and minor version < 0 or > 2
       *          - Forward-compatible flag set and major version < 3"
       */
      if (ctx->ClientMajorVersion < 1 || ctx->ClientMinorVersion < 0)
         err = EGL_BAD_MATCH;

      switch (ctx->ClientMajorVersion) {
      case 1:
         if (ctx->ClientMinorVersion > 5 ||
             (ctx->Flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR) != 0)
            err = EGL_BAD_MATCH;
         break;

      case 2:
         if (ctx->ClientMinorVersion > 1 ||
             (ctx->Flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR) != 0)
            err = EGL_BAD_MATCH;
         break;

      case 3:
         /* Note: The text above is incorrect.  There *is* an OpenGL 3.3!
          */
         if (ctx->ClientMinorVersion > 3)
            err = EGL_BAD_MATCH;
         break;

      case 4:
      default:
         /* Don't put additional version checks here.  We don't know that
          * there won't be versions > 4.2.
          */
         break;
      }
   } else if (api == EGL_OPENGL_ES_API) {
      /* The EGL_KHR_create_context spec says:
       *
       *     "* If an OpenGL ES context is requested and the values for
       *        attributes EGL_CONTEXT_MAJOR_VERSION_KHR and
       *        EGL_CONTEXT_MINOR_VERSION_KHR specify an OpenGL ES version that
       *        is not defined, than an EGL_BAD_MATCH error is generated.
       *
       *        ... Examples of invalid combinations of attributes include:
       *
       *          - Major version < 1 or > 2
       *          - Major version == 1 and minor version < 0 or > 1
       *          - Major version == 2 and minor version != 0
       */
      if (ctx->ClientMajorVersion < 1 || ctx->ClientMinorVersion < 0)
         err = EGL_BAD_MATCH;

      switch (ctx->ClientMajorVersion) {
      case 1:
         if (ctx->ClientMinorVersion > 1)
            err = EGL_BAD_MATCH;
         break;

      case 2:
         if (ctx->ClientMinorVersion > 0)
            err = EGL_BAD_MATCH;
         break;

      case 3:
         /* Don't put additional version checks here.  We don't know that
          * there won't be versions > 3.0.
          */
         break;

      default:
         err = EGL_BAD_MATCH;
         break;
      }
   }

   if (ctx->WebGLCompatibility &&
       (api != EGL_OPENGL_ES_API || ctx->ClientMajorVersion < 2))
      err = EGL_BAD_MATCH;

   switch (ctx->ResetNotificationStrategy) {
   case EGL_NO_RESET_NOTIFICATION_KHR:
   case EGL_LOSE_CONTEXT_ON_RESET_KHR:
      break;

   default:
      err = EGL_BAD_ATTRIBUTE;
      if (first_rejected_attr == EGL_NONE) {
         first_rejected_attr =
            EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR;
         detail = "reset-notification-invalid";
      }
      break;
   }

   /* The KHR_no_error spec only applies against OpenGL 2.0+ and
    * OpenGL ES 2.0+. Validate after all attributes are parsed so the
    * result does not depend on whether the no-error token appears before
    * or after the requested client version.
    */
   if (err == EGL_SUCCESS && ctx->NoError &&
       ((api != EGL_OPENGL_API && api != EGL_OPENGL_ES_API) ||
        ctx->ClientMajorVersion < 2)) {
      err = EGL_BAD_ATTRIBUTE;
      if (first_rejected_attr == EGL_NONE) {
         first_rejected_attr = EGL_CONTEXT_OPENGL_NO_ERROR_KHR;
         detail = "no-error-api-version-invalid";
      }
   }

   /* The EGL_KHR_create_context_no_error spec says:
    *
    *    "BAD_MATCH is generated if the EGL_CONTEXT_OPENGL_NO_ERROR_KHR is TRUE
    * at the same time as a debug or robustness context is specified."
    */
   if (err == EGL_SUCCESS && ctx->NoError &&
       (ctx->Flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR ||
        ctx->Flags & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)) {
      err = EGL_BAD_MATCH;
      if (first_rejected_attr == EGL_NONE)
         detail = "no-error-debug-or-robust-conflict";
   }

   if ((ctx->Flags & ~(EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR |
                       EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR |
                       EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)) != 0) {
      err = EGL_BAD_ATTRIBUTE;
      if (first_rejected_attr == EGL_NONE) {
         first_rejected_attr = EGL_CONTEXT_FLAGS_KHR;
         detail = "unknown-context-flags";
      }
   }

   if (err != EGL_SUCCESS && detail[0] == 's')
      detail = "post-parse-validation";
   *first_rejected_attr_out = first_rejected_attr;
   *detail_out = detail;
   return err;
}

/**
 * Initialize the given _EGLContext object to defaults and/or the values
 * in the attrib_list.
 *
 * According to EGL 1.5 Section 3.7:
 *
 *	"EGL_OPENGL_API and EGL_OPENGL_ES_API are interchangeable for all
 *	purposes except eglCreateContext."
 *
 * And since we only support GL and GLES, this is the only place where the
 * bound API matters at all. We look up the current API from the current
 * thread, and stash that in the context we're initializing. Our caller is
 * responsible for determining whether that's an API it supports.
 */
EGLBoolean
_eglInitContext(_EGLContext *ctx, _EGLDisplay *disp, _EGLConfig *conf,
                _EGLContext *share_list, const EGLint *attrib_list)
{
   const EGLenum api = eglQueryAPI();
   EGLint err;
   EGLint first_rejected_attr = EGL_NONE;
   const char *detail = "success";

   if (api == EGL_NONE)
      return _eglError(EGL_BAD_MATCH, "eglCreateContext(no client API)");

   _eglInitResource(&ctx->Resource, sizeof(*ctx), disp);
   ctx->ClientAPI = api;
   ctx->Config = conf;
   ctx->Profile = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;

   ctx->ClientMajorVersion = 1; /* the default, per EGL spec */
   ctx->ClientMinorVersion = 0;
   ctx->Flags = 0;
   ctx->ResetNotificationStrategy = EGL_NO_RESET_NOTIFICATION_KHR;
   ctx->ContextPriority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
   ctx->WebGLCompatibility = EGL_FALSE;
   ctx->ReleaseBehavior = EGL_CONTEXT_RELEASE_BEHAVIOR_FLUSH_KHR;

   err = _eglParseContextAttribList(ctx, disp, attrib_list,
                                    &first_rejected_attr, &detail);
   if (err == EGL_SUCCESS && ctx->Config) {
      EGLint api_bit;

      api_bit = _eglGetContextAPIBit(ctx);
      if (!(ctx->Config->RenderableType & api_bit)) {
         _eglLog(_EGL_DEBUG, "context api is 0x%x while config supports 0x%x",
                 api_bit, ctx->Config->RenderableType);
         err = EGL_BAD_CONFIG;
         detail = "config-renderable-type-mismatch";
      }
   }
   if (err != EGL_SUCCESS) {
      xv6_egl_context_trace_failure(ctx, disp, share_list, attrib_list, err,
                                    first_rejected_attr, detail);
      return _eglError(err, "eglCreateContext");
   }

   /* The EGL_EXT_create_context_robustness spec says:
    *
    *    "Add to the eglCreateContext context creation errors: [...]
    *
    *     * If the reset notification behavior of <share_context> and the
    *       newly created context are different then an EGL_BAD_MATCH error is
    *       generated."
    */
   if (share_list && share_list->ResetNotificationStrategy !=
                        ctx->ResetNotificationStrategy) {
      xv6_egl_context_trace_failure(
         ctx, disp, share_list, attrib_list, EGL_BAD_MATCH, EGL_NONE,
         "share-reset-notification-mismatch");
      return _eglError(
         EGL_BAD_MATCH,
         "eglCreateContext() share list notification strategy mismatch");
   }

   /* The EGL_KHR_create_context_no_error spec says:
    *
    *    "BAD_MATCH is generated if the value of EGL_CONTEXT_OPENGL_NO_ERROR_KHR
    *    used to create <share_context> does not match the value of
    *    EGL_CONTEXT_OPENGL_NO_ERROR_KHR for the context being created."
    */
   if (share_list && share_list->NoError != ctx->NoError) {
      xv6_egl_context_trace_failure(ctx, disp, share_list, attrib_list,
                                    EGL_BAD_MATCH, EGL_NONE,
                                    "share-no-error-mismatch");
      return _eglError(EGL_BAD_MATCH,
                       "eglCreateContext() share list no-error mismatch");
   }

   return EGL_TRUE;
}

static EGLint
_eglQueryContextRenderBuffer(_EGLContext *ctx)
{
   _EGLSurface *surf = ctx->DrawSurface;

   /* From the EGL 1.5 spec:
    *
    *    - If the context is not bound to a surface, then EGL_NONE will be
    *      returned.
    */
   if (!surf)
      return EGL_NONE;

   switch (surf->Type) {
   default:
      UNREACHABLE("bad EGLSurface type");
   case EGL_PIXMAP_BIT:
      /* - If the context is bound to a pixmap surface, then EGL_SINGLE_BUFFER
       *   will be returned.
       */
      return EGL_SINGLE_BUFFER;
   case EGL_PBUFFER_BIT:
      /* - If the context is bound to a pbuffer surface, then EGL_BACK_BUFFER
       *   will be returned.
       */
      return EGL_BACK_BUFFER;
   case EGL_WINDOW_BIT:
      /* - If the context is bound to a window surface, then either
       *   EGL_BACK_BUFFER or EGL_SINGLE_BUFFER may be returned. The value
       *   returned depends on both the buffer requested by the setting of the
       *   EGL_RENDER_BUFFER property of the surface [...], and on the client
       *   API (not all client APIs support single-buffer Rendering to window
       *   surfaces). Some client APIs allow control of whether rendering goes
       *   to the front or back buffer. This client API-specific choice is not
       *   reflected in the returned value, which only describes the buffer
       *   that will be rendered to by default if not overridden by the client
       *   API.
       */
      return surf->ActiveRenderBuffer;
   }
}

EGLBoolean
_eglQueryContext(_EGLContext *c, EGLint attribute, EGLint *value)
{
   _EGLDisplay *disp = c->Resource.Display;
   EGLenum api = c->ClientAPI;

   if (!value)
      return _eglError(EGL_BAD_PARAMETER, "eglQueryContext");

   switch (attribute) {
   case EGL_CONFIG_ID:
      /*
       * From EGL_KHR_no_config_context:
       *
       *    "Querying EGL_CONFIG_ID returns the ID of the EGLConfig with
       *     respect to which the context was created, or zero if created
       *     without respect to an EGLConfig."
       */
      *value = c->Config ? c->Config->ConfigID : 0;
      break;
   case EGL_CONTEXT_CLIENT_VERSION:
      *value = c->ClientMajorVersion;
      break;
   case EGL_CONTEXT_CLIENT_TYPE:
      *value = c->ClientAPI;
      break;
   case EGL_RENDER_BUFFER:
      *value = _eglQueryContextRenderBuffer(c);
      break;
   case EGL_CONTEXT_PRIORITY_LEVEL_IMG:
      *value = c->ContextPriority;
      break;
   case EGL_PROTECTED_CONTENT_EXT:
      if (!disp->Extensions.EXT_protected_content)
         return _eglError(EGL_BAD_ATTRIBUTE, "eglQueryContext");
      *value = c->Protected;
      break;
   case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT:
      if (!disp->Extensions.EXT_create_context_robustness ||
          api != EGL_OPENGL_ES_API)
         return _eglError(EGL_BAD_ATTRIBUTE, "eglQueryContext");
      *value = c->ResetNotificationStrategy;
      break;
   case EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR:
      if (!(disp->Extensions.KHR_create_context && api == EGL_OPENGL_API) &&
          disp->Version < 15)
         return _eglError(EGL_BAD_ATTRIBUTE, "eglQueryContext");
      *value = c->ResetNotificationStrategy;
      break;
   default:
      return _eglError(EGL_BAD_ATTRIBUTE, "eglQueryContext");
   }

   return EGL_TRUE;
}

/**
 * Bind the context to the thread and return the previous context.
 *
 * Note that the context may be NULL.
 */
_EGLContext *
_eglBindContextToThread(_EGLContext *ctx, _EGLThreadInfo *t)
{
   _EGLContext *oldCtx;

   oldCtx = t->CurrentContext;
   if (ctx != oldCtx) {
      if (oldCtx)
         oldCtx->Binding = NULL;
      if (ctx)
         ctx->Binding = t;

      t->CurrentContext = ctx;
   }

   return oldCtx;
}

/**
 * Return true if the given context and surfaces can be made current.
 */
static EGLBoolean
_eglCheckMakeCurrent(_EGLContext *ctx, _EGLSurface *draw, _EGLSurface *read)
{
   _EGLThreadInfo *t = _eglGetCurrentThread();
   _EGLDisplay *disp;

   /* this is easy */
   if (!ctx) {
      if (draw || read)
         return _eglError(EGL_BAD_MATCH, "eglMakeCurrent");
      return EGL_TRUE;
   }

   disp = ctx->Resource.Display;
   if (!disp->Extensions.KHR_surfaceless_context &&
       (draw == NULL || read == NULL))
      return _eglError(EGL_BAD_MATCH, "eglMakeCurrent");

   /*
    * The spec says
    *
    * "If ctx is current to some other thread, or if either draw or read are
    * bound to contexts in another thread, an EGL_BAD_ACCESS error is
    * generated."
    *
    * and
    *
    * "at most one context may be bound to a particular surface at a given
    * time"
    */
   if (ctx->Binding && ctx->Binding != t)
      return _eglError(EGL_BAD_ACCESS, "eglMakeCurrent");
   if (draw && draw->CurrentContext && draw->CurrentContext != ctx) {
      if (draw->CurrentContext->Binding != t)
         return _eglError(EGL_BAD_ACCESS, "eglMakeCurrent");
   }
   if (read && read->CurrentContext && read->CurrentContext != ctx) {
      if (read->CurrentContext->Binding != t)
         return _eglError(EGL_BAD_ACCESS, "eglMakeCurrent");
   }

   /* If the context has a config then it must match that of the two
    * surfaces */
   if (ctx->Config) {
      if ((draw && draw->Config != ctx->Config) ||
          (read && read->Config != ctx->Config))
         return _eglError(EGL_BAD_MATCH, "eglMakeCurrent");
   } else {
      /* Otherwise we must be using the EGL_KHR_no_config_context
       * extension */
      assert(disp->Extensions.KHR_no_config_context);
   }

   return EGL_TRUE;
}

/**
 * Bind the context to the current thread and given surfaces.  Return the
 * previous bound context and surfaces.  The caller should unreference the
 * returned context and surfaces.
 *
 * Making a second call with the resources returned by the first call
 * unsurprisingly undoes the first call, except for the resource reference
 * counts.
 */
EGLBoolean
_eglBindContext(_EGLContext *ctx, _EGLSurface *draw, _EGLSurface *read,
                _EGLContext **old_ctx, _EGLSurface **old_draw,
                _EGLSurface **old_read)
{
   _EGLThreadInfo *t = _eglGetCurrentThread();
   _EGLContext *prev_ctx;
   _EGLSurface *prev_draw, *prev_read;

   if (!_eglCheckMakeCurrent(ctx, draw, read))
      return EGL_FALSE;

   /* increment refcounts before binding */
   _eglGetContext(ctx);
   _eglGetSurface(draw);
   _eglGetSurface(read);

   /* bind the new context */
   prev_ctx = _eglBindContextToThread(ctx, t);

   /* break previous bindings */
   if (prev_ctx) {
      prev_draw = prev_ctx->DrawSurface;
      prev_read = prev_ctx->ReadSurface;

      if (prev_draw)
         prev_draw->CurrentContext = NULL;
      if (prev_read)
         prev_read->CurrentContext = NULL;

      prev_ctx->DrawSurface = NULL;
      prev_ctx->ReadSurface = NULL;
   } else {
      prev_draw = prev_read = NULL;
   }

   /* establish new bindings */
   if (ctx) {
      if (draw)
         draw->CurrentContext = ctx;
      if (read)
         read->CurrentContext = ctx;

      ctx->DrawSurface = draw;
      ctx->ReadSurface = read;
   }

   assert(old_ctx && old_draw && old_read);
   *old_ctx = prev_ctx;
   *old_draw = prev_draw;
   *old_read = prev_read;

   return EGL_TRUE;
}
