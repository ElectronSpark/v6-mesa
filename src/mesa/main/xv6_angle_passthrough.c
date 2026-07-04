/*
 * Copyright (C) 2026 xv6-os contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "util/glheader.h"
#include <string.h>
#include "context.h"
#include "errors.h"
#include "glformats.h"
#include "image.h"
#include "pbo.h"
#include "texcompress.h"
#include "teximage.h"
#include "api_exec_decl.h"

void GLAPIENTRY
_mesa_RequestExtensionANGLE(const GLchar *name)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!name) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glRequestExtensionANGLE(name=NULL)");
      return;
   }

   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glRequestExtensionANGLE(%s is not requestable)", name);
}

struct robust_error_state {
   GLenum16 error;
   const char *debug_fmt;
   GLuint debug_count;
};

static void
robust_error_snapshot(struct gl_context *ctx, struct robust_error_state *state)
{
   state->error = ctx->ErrorValue;
   state->debug_fmt = ctx->ErrorDebugFmtString;
   state->debug_count = ctx->ErrorDebugCount;

   if (state->error != GL_NO_ERROR)
      ctx->ErrorValue = GL_NO_ERROR;
}

static bool
robust_error_generated(struct gl_context *ctx,
                       const struct robust_error_state *state)
{
   bool generated = ctx->ErrorValue != GL_NO_ERROR ||
      ctx->ErrorDebugFmtString != state->debug_fmt ||
      ctx->ErrorDebugCount != state->debug_count;

   if (state->error != GL_NO_ERROR) {
      ctx->ErrorValue = state->error;
      ctx->ErrorDebugFmtString = state->debug_fmt;
      ctx->ErrorDebugCount = state->debug_count;
   }

   return generated;
}

static bool
robust_param_count_ok(struct gl_context *ctx, GLsizei paramCount,
                      const char *func)
{
   if (paramCount < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(paramCount=%d)",
                  func, paramCount);
      return false;
   }
   if (paramCount == 0) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(paramCount=0)", func);
      return false;
   }
   return true;
}

static GLsizei
robust_get_count(struct gl_context *ctx, GLenum pname)
{
   switch (pname) {
   case GL_ALIASED_LINE_WIDTH_RANGE:
   case GL_ALIASED_POINT_SIZE_RANGE:
   case GL_DEPTH_RANGE:
   case GL_MAX_VIEWPORT_DIMS:
   case GL_VIEWPORT_BOUNDS_RANGE:
      return 2;
   case GL_CURRENT_COLOR:
   case GL_CURRENT_SECONDARY_COLOR:
   case GL_CURRENT_TEXTURE_COORDS:
   case GL_FOG_COLOR:
   case GL_LIGHT_MODEL_AMBIENT:
   case GL_BLEND_COLOR:
   case GL_COLOR_CLEAR_VALUE:
   case GL_COLOR_WRITEMASK:
   case GL_SCISSOR_BOX:
   case GL_VIEWPORT:
      return 4;
   case GL_MODELVIEW_MATRIX:
   case GL_PROJECTION_MATRIX:
   case GL_TEXTURE_MATRIX:
      return 16;
   case GL_COMPRESSED_TEXTURE_FORMATS:
      return _mesa_get_compressed_formats(ctx, NULL);
   case GL_SHADER_BINARY_FORMATS:
      return ctx->Const.NumShaderBinaryFormats;
   default:
      return 1;
   }
}

#define ROBUST_GET_BODY(TYPE, NAME, BASE)                                \
   do {                                                                  \
      GET_CURRENT_CONTEXT(ctx);                                          \
      if (!robust_param_count_ok(ctx, paramCount, "gl" #NAME))          \
         return;                                                         \
      if (!data) {                                                        \
         _mesa_error(ctx, GL_INVALID_VALUE, "gl" #NAME "(data=NULL)");   \
         return;                                                         \
      }                                                                  \
      GLsizei count = robust_get_count(ctx, pname);                       \
      if (paramCount < count) {                                          \
         _mesa_error(ctx, GL_INVALID_OPERATION,                          \
                     "gl" #NAME "(paramCount too small)");             \
         return;                                                         \
      }                                                                  \
      TYPE tmp[100];                                                     \
      struct robust_error_state error_state;                             \
      robust_error_snapshot(ctx, &error_state);                          \
      BASE(pname, tmp);                                                  \
      if (robust_error_generated(ctx, &error_state))                     \
         return;                                                         \
      memcpy(data, tmp, count * sizeof(tmp[0]));                         \
      if (length)                                                        \
         *length = count;                                                \
   } while (0)

void GLAPIENTRY
_mesa_GetBooleanvRobustANGLE(GLenum pname, GLsizei paramCount,
                             GLsizei *length, GLboolean *data)
{
   ROBUST_GET_BODY(GLboolean, GetBooleanvRobustANGLE, _mesa_GetBooleanv);
}

void GLAPIENTRY
_mesa_GetFloatvRobustANGLE(GLenum pname, GLsizei paramCount,
                           GLsizei *length, GLfloat *data)
{
   ROBUST_GET_BODY(GLfloat, GetFloatvRobustANGLE, _mesa_GetFloatv);
}

void GLAPIENTRY
_mesa_GetIntegervRobustANGLE(GLenum pname, GLsizei paramCount,
                             GLsizei *length, GLint *data)
{
   ROBUST_GET_BODY(GLint, GetIntegervRobustANGLE, _mesa_GetIntegerv);
}

void GLAPIENTRY
_mesa_GetInteger64vRobustANGLE(GLenum pname, GLsizei paramCount,
                               GLsizei *length, GLint64 *data)
{
   ROBUST_GET_BODY(GLint64, GetInteger64vRobustANGLE, _mesa_GetInteger64v);
}

#undef ROBUST_GET_BODY

void GLAPIENTRY
_mesa_GetIntegeri_vRobustANGLE(GLenum target, GLuint index,
                               GLsizei paramCount, GLsizei *length,
                               GLint *data)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!robust_param_count_ok(ctx, paramCount, "glGetIntegeri_vRobustANGLE"))
      return;
   if (!data) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGetIntegeri_vRobustANGLE(data=NULL)");
      return;
   }

   GLsizei count = robust_get_count(ctx, target);
   if (paramCount < count) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glGetIntegeri_vRobustANGLE(paramCount too small)");
      return;
   }

   GLint tmp[100];
   struct robust_error_state error_state;
   robust_error_snapshot(ctx, &error_state);
   _mesa_GetIntegeri_v(target, index, tmp);
   if (robust_error_generated(ctx, &error_state))
      return;
   memcpy(data, tmp, count * sizeof(tmp[0]));
   if (length)
      *length = count;
}

void GLAPIENTRY
_mesa_GetInteger64i_vRobustANGLE(GLenum target, GLuint index,
                                 GLsizei paramCount, GLsizei *length,
                                 GLint64 *data)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!robust_param_count_ok(ctx, paramCount, "glGetInteger64i_vRobustANGLE"))
      return;
   if (!data) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetInteger64i_vRobustANGLE(data=NULL)");
      return;
   }

   GLsizei count = robust_get_count(ctx, target);
   if (paramCount < count) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glGetInteger64i_vRobustANGLE(paramCount too small)");
      return;
   }

   GLint64 tmp[100];
   struct robust_error_state error_state;
   robust_error_snapshot(ctx, &error_state);
   _mesa_GetInteger64i_v(target, index, tmp);
   if (robust_error_generated(ctx, &error_state))
      return;
   memcpy(data, tmp, count * sizeof(tmp[0]));
   if (length)
      *length = count;
}

void GLAPIENTRY
_mesa_GetUniformfvRobustANGLE(GLuint program, GLint location,
                              GLsizei bufSize, GLsizei *length,
                              GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGetUniformfvRobustANGLE(bufSize)");
      return;
   }
   if (bufSize > 0 && !params) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetUniformfvRobustANGLE(params=NULL)");
      return;
   }

   struct robust_error_state error_state;
   robust_error_snapshot(ctx, &error_state);
   _mesa_GetnUniformfvARB(program, location, bufSize, params);
   if (robust_error_generated(ctx, &error_state))
      return;
}

void GLAPIENTRY
_mesa_GetUniformivRobustANGLE(GLuint program, GLint location,
                              GLsizei bufSize, GLsizei *length,
                              GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGetUniformivRobustANGLE(bufSize)");
      return;
   }
   if (bufSize > 0 && !params) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetUniformivRobustANGLE(params=NULL)");
      return;
   }

   struct robust_error_state error_state;
   robust_error_snapshot(ctx, &error_state);
   _mesa_GetnUniformivARB(program, location, bufSize, params);
   if (robust_error_generated(ctx, &error_state))
      return;
}

void GLAPIENTRY
_mesa_GetUniformuivRobustANGLE(GLuint program, GLint location,
                               GLsizei bufSize, GLsizei *length,
                               GLuint *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGetUniformuivRobustANGLE(bufSize)");
      return;
   }
   if (bufSize > 0 && !params) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetUniformuivRobustANGLE(params=NULL)");
      return;
   }

   struct robust_error_state error_state;
   robust_error_snapshot(ctx, &error_state);
   _mesa_GetnUniformuivARB(program, location, bufSize, params);
   if (robust_error_generated(ctx, &error_state))
      return;
}

void GLAPIENTRY
_mesa_ReadPixelsRobustANGLE(GLint x, GLint y, GLsizei width, GLsizei height,
                            GLenum format, GLenum type, GLsizei bufSize,
                            GLsizei *length, GLsizei *columns, GLsizei *rows,
                            GLvoid *pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_pixelstore_attrib clipped = ctx->Pack;
   GLint clipped_x = x;
   GLint clipped_y = y;
   GLsizei clipped_width = width;
   GLsizei clipped_height = height;
   GLboolean clipped_ok = GL_FALSE;

   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glReadPixelsRobustANGLE(bufSize)");
      return;
   }

   if (width >= 0 && height >= 0)
      clipped_ok = _mesa_clip_readpixels(ctx, &clipped_x, &clipped_y,
                                         &clipped_width, &clipped_height,
                                         &clipped);

   struct robust_error_state error_state;
   robust_error_snapshot(ctx, &error_state);
   _mesa_ReadnPixelsARB(x, y, width, height, format, type, bufSize, pixels);
   if (robust_error_generated(ctx, &error_state))
      return;

   if (columns)
      *columns = clipped_ok ? clipped_width : 0;
   if (rows)
      *rows = clipped_ok ? clipped_height : 0;
   if (length) {
      GLint bpp = clipped_ok ? _mesa_bytes_per_pixel(format, type) : 0;
      *length = bpp > 0 ? clipped_width * clipped_height * bpp : 0;
   }
}

void GLAPIENTRY
_mesa_TexImage2DRobustANGLE(GLenum target, GLint level, GLint internalformat,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, GLsizei bufSize,
                            const GLvoid *pixels)
{
   GET_CURRENT_CONTEXT(ctx);

   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glTexImage2DRobustANGLE(bufSize)");
      return;
   }
   _mesa_TexImage2D_with_client_memsize(target, level, internalformat,
                                        width, height, border, format, type,
                                        bufSize, pixels,
                                        "glTexImage2DRobustANGLE");
}

void GLAPIENTRY
_mesa_TexSubImage2DRobustANGLE(GLenum target, GLint level, GLint xoffset,
                               GLint yoffset, GLsizei width, GLsizei height,
                               GLenum format, GLenum type, GLsizei bufSize,
                               const GLvoid *pixels)
{
   GET_CURRENT_CONTEXT(ctx);

   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glTexSubImage2DRobustANGLE(bufSize)");
      return;
   }
   _mesa_TexSubImage2D_with_client_memsize(target, level, xoffset, yoffset,
                                           width, height, format, type,
                                           bufSize, pixels,
                                           "glTexSubImage2DRobustANGLE");
}
