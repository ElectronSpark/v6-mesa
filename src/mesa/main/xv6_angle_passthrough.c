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
#include "texobj.h"
#include "uniforms.h"
#include "../state_tracker/st_cb_copyimage.h"
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

static bool
chromium_copy_dest_target(GLenum destTarget, GLenum *objectTarget)
{
   switch (destTarget) {
   case GL_TEXTURE_2D:
      *objectTarget = GL_TEXTURE_2D;
      return true;
   case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
      *objectTarget = GL_TEXTURE_CUBE_MAP;
      return true;
   default:
      return false;
   }
}

static bool
chromium_copy_valid_source_format(GLenum format)
{
   switch (format) {
   case GL_ALPHA:
   case GL_LUMINANCE:
   case GL_LUMINANCE_ALPHA:
   case GL_RED:
   case GL_RG:
   case GL_RGB:
   case GL_RGBA:
   case GL_R8:
   case GL_RG8:
   case GL_RGB8:
   case GL_RGBA8:
   case GL_BGRA_EXT:
      return true;
   default:
      return false;
   }
}

static bool
chromium_copy_dest_format_type(GLenum internalFormat, GLenum destType,
                               GLenum *format, bool *integer)
{
   *integer = false;

   switch (internalFormat) {
   case GL_ALPHA:
   case GL_LUMINANCE:
   case GL_LUMINANCE_ALPHA:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = internalFormat;
      return true;
   case GL_RGB:
   case GL_RGB8:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RGB;
      return true;
   case GL_RGBA:
   case GL_RGBA8:
   case GL_BGRA_EXT:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = internalFormat == GL_BGRA_EXT ? GL_BGRA_EXT : GL_RGBA;
      return true;
   case GL_RGB565:
      if (destType != GL_UNSIGNED_SHORT_5_6_5)
         return false;
      *format = GL_RGB;
      return true;
   case GL_RGBA4:
      if (destType != GL_UNSIGNED_SHORT_4_4_4_4)
         return false;
      *format = GL_RGBA;
      return true;
   case GL_RGB5_A1:
      if (destType != GL_UNSIGNED_SHORT_5_5_5_1)
         return false;
      *format = GL_RGBA;
      return true;
   case GL_R8:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RED;
      return true;
   case GL_RG8:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RG;
      return true;
   case GL_SRGB8:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RGB;
      return true;
   case GL_SRGB8_ALPHA8:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RGBA;
      return true;
   case GL_RGBA16F:
      if (destType != GL_HALF_FLOAT)
         return false;
      *format = GL_RGBA;
      return true;
   case GL_RGBA32F:
      if (destType != GL_FLOAT)
         return false;
      *format = GL_RGBA;
      return true;
   case GL_R8UI:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RED_INTEGER;
      *integer = true;
      return true;
   case GL_RG8UI:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RG_INTEGER;
      *integer = true;
      return true;
   case GL_RGB8UI:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RGB_INTEGER;
      *integer = true;
      return true;
   case GL_RGBA8UI:
      if (destType != GL_UNSIGNED_BYTE)
         return false;
      *format = GL_RGBA_INTEGER;
      *integer = true;
      return true;
   default:
      return false;
   }
}

static struct gl_texture_object *
chromium_copy_lookup_texture(struct gl_context *ctx, GLuint id,
                             const char *func, const char *which)
{
   struct gl_texture_object *obj = _mesa_lookup_texture(ctx, id);

   if (!obj || obj->Target == 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(%s texture)", func, which);
      return NULL;
   }
   return obj;
}

static struct gl_texture_image *
chromium_copy_texture_image(struct gl_context *ctx,
                            struct gl_texture_object *obj, GLenum target,
                            GLint level, const char *func, const char *which)
{
   struct gl_texture_image *image;

   if (level < 0 || (_mesa_is_gles(ctx) && !_mesa_is_gles3(ctx) && level != 0)) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(%s level=%d)", func, which, level);
      return NULL;
   }

   image = _mesa_select_tex_image(obj, target, level);
   if (!image || image->Width <= 0 || image->Height <= 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(undefined %s level)", func, which);
      return NULL;
   }
   return image;
}

static bool
chromium_copy_check_bounds(struct gl_context *ctx,
                           const struct gl_texture_image *srcImage,
                           const struct gl_texture_image *dstImage,
                           GLint xoffset, GLint yoffset, GLint x, GLint y,
                           GLsizei width, GLsizei height, const char *func)
{
   if (width < 0 || height < 0 || xoffset < 0 || yoffset < 0 || x < 0 || y < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(negative bounds)", func);
      return false;
   }
   if (x > srcImage->Width || y > srcImage->Height ||
       width > srcImage->Width - x || height > srcImage->Height - y ||
       xoffset > dstImage->Width || yoffset > dstImage->Height ||
       width > dstImage->Width - xoffset ||
       height > dstImage->Height - yoffset) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(out of bounds)", func);
      return false;
   }
   return true;
}

static GLuint
chromium_copy_compile_shader(struct gl_context *ctx, GLenum type,
                             const GLchar *source)
{
   GLuint shader = _mesa_CreateShader(type);
   GLint ok = GL_FALSE;

   if (!shader)
      return 0;

   _mesa_ShaderSource(shader, 1, &source, NULL);
   _mesa_CompileShader(shader);
   _mesa_GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      _mesa_DeleteShader(shader);
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glCopyTextureCHROMIUM(internal shader compile)");
      return 0;
   }
   return shader;
}

static GLuint
chromium_copy_program(struct gl_context *ctx, bool external_source,
                      bool integer_dest)
{
   static const GLchar vs_source[] =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_tex;\n"
      "varying vec2 v_tex;\n"
      "void main() {\n"
      "   gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "   v_tex = a_tex;\n"
      "}\n";
   static const GLchar fs_source[] =
      "precision mediump float;\n"
      "uniform sampler2D u_tex;\n"
      "uniform int u_premul;\n"
      "uniform int u_unmul;\n"
      "varying vec2 v_tex;\n"
      "void main() {\n"
      "   vec4 c = texture2D(u_tex, v_tex);\n"
      "   if (u_premul == 1 && u_unmul == 0) {\n"
      "      c.rgb *= c.a;\n"
      "   } else if (u_unmul == 1 && u_premul == 0) {\n"
      "      c.rgb = c.a == 0.0 ? vec3(0.0) : clamp(c.rgb / c.a, 0.0, 1.0);\n"
      "   }\n"
      "   gl_FragColor = c;\n"
      "}\n";
   static const GLchar fs_external_source[] =
      "#extension GL_OES_EGL_image_external : require\n"
      "precision mediump float;\n"
      "uniform samplerExternalOES u_tex;\n"
      "uniform int u_premul;\n"
      "uniform int u_unmul;\n"
      "varying vec2 v_tex;\n"
      "void main() {\n"
      "   vec4 c = texture2D(u_tex, v_tex);\n"
      "   if (u_premul == 1 && u_unmul == 0) {\n"
      "      c.rgb *= c.a;\n"
      "   } else if (u_unmul == 1 && u_premul == 0) {\n"
      "      c.rgb = c.a == 0.0 ? vec3(0.0) : clamp(c.rgb / c.a, 0.0, 1.0);\n"
      "   }\n"
      "   gl_FragColor = c;\n"
      "}\n";
   static const GLchar vs_es3_source[] =
      "#version 300 es\n"
      "layout(location = 0) in vec2 a_pos;\n"
      "layout(location = 1) in vec2 a_tex;\n"
      "out vec2 v_tex;\n"
      "void main() {\n"
      "   gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "   v_tex = a_tex;\n"
      "}\n";
   static const GLchar fs_external_integer_source[] =
      "#version 300 es\n"
      "#extension GL_OES_EGL_image_external_essl3 : require\n"
      "precision mediump float;\n"
      "uniform samplerExternalOES u_tex;\n"
      "in vec2 v_tex;\n"
      "out highp uvec4 out_color;\n"
      "void main() {\n"
      "   vec4 c = clamp(texture(u_tex, v_tex), 0.0, 1.0);\n"
      "   out_color = uvec4(c * 255.0 + 0.5);\n"
      "}\n";
   GLuint vs = chromium_copy_compile_shader(ctx, GL_VERTEX_SHADER,
                                            integer_dest ? vs_es3_source :
                                            vs_source);
   GLuint fs = chromium_copy_compile_shader(ctx, GL_FRAGMENT_SHADER,
                                            integer_dest ?
                                            fs_external_integer_source :
                                            (external_source ?
                                             fs_external_source : fs_source));
   GLuint program = 0;
   GLint ok = GL_FALSE;

   if (!vs || !fs)
      goto out;

   program = _mesa_CreateProgram();
   if (!program)
      goto out;

   _mesa_AttachShader(program, vs);
   _mesa_AttachShader(program, fs);
   _mesa_LinkProgram(program);
   _mesa_GetProgramiv(program, GL_LINK_STATUS, &ok);
   if (!ok) {
      _mesa_DeleteProgram(program);
      program = 0;
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glCopyTextureCHROMIUM(internal shader link)");
   }

out:
   if (vs)
      _mesa_DeleteShader(vs);
   if (fs)
      _mesa_DeleteShader(fs);
   return program;
}

static bool
chromium_copy_draw(struct gl_context *ctx, GLenum srcTarget, GLuint sourceId,
                   GLint sourceLevel,
                   GLenum destTarget, GLuint destId, GLint destLevel,
                   GLint xoffset, GLint yoffset, GLint x, GLint y,
                   GLsizei width, GLsizei height, GLsizei srcWidth,
                   GLsizei srcHeight, GLboolean unpackFlipY,
                   GLboolean unpackPremultiplyAlpha,
                   GLboolean unpackUnmultiplyAlpha, bool integer_dest)
{
   const GLfloat u0 = (GLfloat) x / (GLfloat) srcWidth;
   const GLfloat u1 = (GLfloat) (x + width) / (GLfloat) srcWidth;
   const GLfloat va = (GLfloat) y / (GLfloat) srcHeight;
   const GLfloat vb = (GLfloat) (y + height) / (GLfloat) srcHeight;
   const GLfloat v0 = unpackFlipY ? vb : va;
   const GLfloat v1 = unpackFlipY ? va : vb;
   const GLfloat vertices[] = {
      -1.0f, -1.0f, u0, v0,
       1.0f, -1.0f, u1, v0,
      -1.0f,  1.0f, u0, v1,
       1.0f,  1.0f, u1, v1,
   };
   GLint oldDrawFbo = 0, oldReadFbo = 0, oldViewport[4] = { 0 };
   GLint oldProgram = 0, oldArrayBuffer = 0, oldActiveTexture = 0;
   GLint oldTexture2D = 0, oldTextureCube = 0, oldTextureExternal = 0;
   GLint oldVao = 0;
   GLint oldBaseLevel = 0, oldMaxLevel = 0;
   GLboolean oldScissor = GL_FALSE, oldBlend = GL_FALSE;
   GLboolean oldDepth = GL_FALSE, oldStencil = GL_FALSE, oldCull = GL_FALSE;
   GLboolean oldDepthMask = GL_TRUE;
   GLboolean oldColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
   GLuint fbo = 0, vbo = 0, vao = 0, program = 0;
   GLenum dstObjectTarget = 0;
   GLint loc;
   bool changedSourceLevel = false;
   bool ok = false;

   _mesa_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
   _mesa_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
   _mesa_GetIntegerv(GL_VIEWPORT, oldViewport);
   _mesa_GetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
   _mesa_GetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
   _mesa_GetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
   _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture2D);
   _mesa_GetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &oldTextureCube);
   _mesa_GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &oldTextureExternal);
   if (_mesa_is_gles3(ctx))
      _mesa_GetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);

   oldScissor = (ctx->Scissor.EnableFlags & 1) != 0;
   oldBlend = (ctx->Color.BlendEnabled & 1) != 0;
   oldDepth = ctx->Depth.Test;
   oldStencil = ctx->Stencil.Enabled;
   oldCull = ctx->Polygon.CullFlag;
   oldDepthMask = ctx->Depth.Mask;
   _mesa_GetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

   program = chromium_copy_program(ctx, srcTarget == GL_TEXTURE_EXTERNAL_OES,
                                   integer_dest);
   if (!program)
      goto out;

   chromium_copy_dest_target(destTarget, &dstObjectTarget);

   _mesa_GenFramebuffers(1, &fbo);
   _mesa_BindFramebuffer(GL_FRAMEBUFFER, fbo);
   _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              destTarget, destId, destLevel);
   if (_mesa_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glCopyTextureCHROMIUM(incomplete destination framebuffer)");
      goto out;
   }

   if (_mesa_is_gles3(ctx)) {
      _mesa_GenVertexArrays(1, &vao);
      _mesa_BindVertexArray(vao);
   }
   _mesa_GenBuffers(1, &vbo);
   _mesa_BindBuffer(GL_ARRAY_BUFFER, vbo);
   _mesa_BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

   _mesa_UseProgram(program);
   loc = _mesa_GetUniformLocation(program, "u_tex");
   if (loc >= 0)
      _mesa_Uniform1i(loc, 0);
   loc = _mesa_GetUniformLocation(program, "u_premul");
   if (loc >= 0)
      _mesa_Uniform1i(loc, unpackPremultiplyAlpha ? 1 : 0);
   loc = _mesa_GetUniformLocation(program, "u_unmul");
   if (loc >= 0)
      _mesa_Uniform1i(loc, unpackUnmultiplyAlpha ? 1 : 0);

   _mesa_ActiveTexture(GL_TEXTURE0);
   _mesa_BindTexture(srcTarget, sourceId);
   if (_mesa_is_gles3(ctx) && srcTarget != GL_TEXTURE_EXTERNAL_OES) {
      _mesa_GetTexParameteriv(srcTarget, GL_TEXTURE_BASE_LEVEL, &oldBaseLevel);
      _mesa_GetTexParameteriv(srcTarget, GL_TEXTURE_MAX_LEVEL, &oldMaxLevel);
      _mesa_TexParameteri(srcTarget, GL_TEXTURE_BASE_LEVEL, sourceLevel);
      _mesa_TexParameteri(srcTarget, GL_TEXTURE_MAX_LEVEL, sourceLevel);
      changedSourceLevel = true;
   }
   _mesa_Disable(GL_SCISSOR_TEST);
   _mesa_Disable(GL_BLEND);
   _mesa_Disable(GL_DEPTH_TEST);
   _mesa_Disable(GL_STENCIL_TEST);
   _mesa_Disable(GL_CULL_FACE);
   _mesa_DepthMask(GL_FALSE);
   _mesa_ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   _mesa_Viewport(xoffset, yoffset, width, height);

   _mesa_EnableVertexAttribArray(0);
   _mesa_EnableVertexAttribArray(1);
   _mesa_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                             (const GLvoid *) 0);
   _mesa_VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                             (const GLvoid *) (uintptr_t) (2 * sizeof(GLfloat)));
   _mesa_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   ok = ctx->ErrorValue == GL_NO_ERROR;

out:
   if (vbo)
      _mesa_DeleteBuffers(1, &vbo);
   if (vao)
      _mesa_DeleteVertexArrays(1, &vao);
   if (fbo)
      _mesa_DeleteFramebuffers(1, &fbo);
   if (program)
      _mesa_DeleteProgram(program);

   if (changedSourceLevel) {
      _mesa_ActiveTexture(GL_TEXTURE0);
      _mesa_BindTexture(srcTarget, sourceId);
      _mesa_TexParameteri(srcTarget, GL_TEXTURE_BASE_LEVEL, oldBaseLevel);
      _mesa_TexParameteri(srcTarget, GL_TEXTURE_MAX_LEVEL, oldMaxLevel);
   }

   _mesa_UseProgram(oldProgram);
   _mesa_ActiveTexture(GL_TEXTURE0);
   _mesa_BindTexture(GL_TEXTURE_2D, oldTexture2D);
   _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, oldTextureCube);
   _mesa_BindTexture(GL_TEXTURE_EXTERNAL_OES, oldTextureExternal);
   _mesa_ActiveTexture(oldActiveTexture);
   _mesa_BindBuffer(GL_ARRAY_BUFFER, oldArrayBuffer);
   if (_mesa_is_gles3(ctx))
      _mesa_BindVertexArray(oldVao);
   _mesa_BindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFbo);
   _mesa_BindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFbo);
   _mesa_Viewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
   if (oldScissor)
      _mesa_Enable(GL_SCISSOR_TEST);
   if (oldBlend)
      _mesa_Enable(GL_BLEND);
   if (oldDepth)
      _mesa_Enable(GL_DEPTH_TEST);
   _mesa_DepthMask(oldDepthMask);
   _mesa_ColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2],
                   oldColorMask[3]);
   if (oldStencil)
      _mesa_Enable(GL_STENCIL_TEST);
   if (oldCull)
      _mesa_Enable(GL_CULL_FACE);

   return ok;
}

static bool
chromium_copy_needs_temp_dest(GLenum internalFormat, GLint destLevel)
{
   if (destLevel > 0)
      return true;

   switch (internalFormat) {
   case GL_ALPHA:
   case GL_LUMINANCE:
   case GL_LUMINANCE_ALPHA:
      return true;
   default:
      return false;
   }
}

static bool
chromium_copy_draw_via_temp(struct gl_context *ctx, GLenum srcTarget,
                            GLuint sourceId, GLint sourceLevel,
                            GLenum destTarget, GLuint destId,
                            GLint destLevel, GLint xoffset, GLint yoffset,
                            GLint x, GLint y, GLsizei width, GLsizei height,
                            GLsizei srcWidth, GLsizei srcHeight,
                           GLboolean unpackFlipY,
                           GLboolean unpackPremultiplyAlpha,
                            GLboolean unpackUnmultiplyAlpha)
{
   GLint oldReadFbo = 0, oldDrawFbo = 0, oldActiveTexture = 0;
   GLint oldTexture2D = 0, oldTextureCube = 0;
   GLuint tempTex = 0, tempFbo = 0;
   GLenum dstObjectTarget = 0;
   bool ok = false;

   _mesa_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
   _mesa_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
   _mesa_GetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
   _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture2D);
   _mesa_GetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &oldTextureCube);

   _mesa_ActiveTexture(GL_TEXTURE0);
   _mesa_GenTextures(1, &tempTex);
   _mesa_BindTexture(GL_TEXTURE_2D, tempTex);
   _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   _mesa_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   _mesa_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, NULL);
   if (ctx->ErrorValue != GL_NO_ERROR)
      goto out;

   if (!chromium_copy_draw(ctx, srcTarget, sourceId, sourceLevel,
                           GL_TEXTURE_2D, tempTex, 0, 0, 0, x, y, width,
                           height, srcWidth, srcHeight, unpackFlipY,
                           unpackPremultiplyAlpha,
                           unpackUnmultiplyAlpha, false))
      goto out;

   chromium_copy_dest_target(destTarget, &dstObjectTarget);
   _mesa_GenFramebuffers(1, &tempFbo);
   _mesa_BindFramebuffer(GL_READ_FRAMEBUFFER, tempFbo);
   _mesa_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, tempTex, 0);
   if (_mesa_CheckFramebufferStatus(GL_READ_FRAMEBUFFER) !=
       GL_FRAMEBUFFER_COMPLETE) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glCopyTextureCHROMIUM(incomplete temporary framebuffer)");
      goto out;
   }

   _mesa_ActiveTexture(GL_TEXTURE0);
   _mesa_BindTexture(dstObjectTarget, destId);
   _mesa_CopyTexSubImage2D(destTarget, destLevel, xoffset, yoffset,
                           0, 0, width, height);
   ok = ctx->ErrorValue == GL_NO_ERROR;

out:
   if (tempFbo)
      _mesa_DeleteFramebuffers(1, &tempFbo);
   if (tempTex)
      _mesa_DeleteTextures(1, &tempTex);
   _mesa_BindTexture(GL_TEXTURE_2D, oldTexture2D);
   _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, oldTextureCube);
   _mesa_ActiveTexture(oldActiveTexture);
   _mesa_BindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFbo);
   _mesa_BindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFbo);
   return ok;
}

static void
chromium_copy_texture_common(GLuint sourceId, GLint sourceLevel,
                             GLenum destTarget, GLuint destId,
                             GLint destLevel, GLenum internalFormat,
                             GLenum destType, GLint xoffset, GLint yoffset,
                             GLint x, GLint y, GLsizei width, GLsizei height,
                             GLboolean unpackFlipY,
                             GLboolean unpackPremultiplyAlpha,
                             GLboolean unpackUnmultiplyAlpha, bool full_copy,
                             const char *func)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_texture_object *srcObj, *dstObj;
   struct gl_texture_image *srcImage, *dstImage;
   GLenum dstObjectTarget = 0;
   GLenum destFormat = GL_NONE;
   bool destInteger = false;

   if (!_mesa_has_CHROMIUM_copy_texture(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(extension unavailable)", func);
      return;
   }
   if (!chromium_copy_dest_target(destTarget, &dstObjectTarget)) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(destTarget)", func);
      return;
   }
   if (full_copy &&
       !chromium_copy_dest_format_type(internalFormat, destType, &destFormat,
                                       &destInteger)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(internalFormat/type)", func);
      return;
   }

   srcObj = chromium_copy_lookup_texture(ctx, sourceId, func, "source");
   dstObj = chromium_copy_lookup_texture(ctx, destId, func, "destination");
   if (!srcObj || !dstObj)
      return;
   if ((srcObj->Target != GL_TEXTURE_2D &&
        srcObj->Target != GL_TEXTURE_EXTERNAL_OES) ||
       dstObj->Target != dstObjectTarget) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(texture target)", func);
      return;
   }
   if (sourceId == destId && sourceLevel == destLevel) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(same texture level)", func);
      return;
   }
   if (srcObj->Target == GL_TEXTURE_EXTERNAL_OES && sourceLevel != 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(external source level)", func);
      return;
   }
   if (full_copy && dstObj->Immutable) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(immutable destination)", func);
      return;
   }

   srcImage = chromium_copy_texture_image(ctx, srcObj, srcObj->Target,
                                          sourceLevel, func, "source");
   if (!srcImage)
      return;
   if (!chromium_copy_valid_source_format(srcImage->_BaseFormat)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(source format)", func);
      return;
   }
   if (full_copy) {
      GLint oldActiveTexture = 0;
      GLint oldTexture2D = 0;
      GLint oldTextureCube = 0;

      if (srcObj->Target == GL_TEXTURE_EXTERNAL_OES && destInteger &&
          (!_mesa_is_gles3(ctx) || !ctx->Extensions.OES_EGL_image_external)) {
         _mesa_error(ctx, GL_INVALID_OPERATION, "%s(external integer dest)",
                     func);
         return;
      }

      _mesa_GetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
      _mesa_GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture2D);
      _mesa_GetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &oldTextureCube);
      _mesa_ActiveTexture(GL_TEXTURE0);
      _mesa_BindTexture(dstObjectTarget, destId);
      for (GLint level = destLevel - 1; level >= 0; level--) {
         GLsizei levelWidth = srcImage->Width << (destLevel - level);
         GLsizei levelHeight = srcImage->Height << (destLevel - level);

         _mesa_TexImage2D(destTarget, level, internalFormat, levelWidth,
                          levelHeight, 0, destFormat, destType, NULL);
      }
      for (GLint level = destLevel + 1,
                 levelWidth = MAX2(srcImage->Width / 2, 1),
                 levelHeight = MAX2(srcImage->Height / 2, 1);
           levelWidth >= 1 && levelHeight >= 1;
           level++, levelWidth = levelWidth > 1 ? levelWidth / 2 : 0,
                 levelHeight = levelHeight > 1 ? levelHeight / 2 : 0) {
         _mesa_TexImage2D(destTarget, level, internalFormat, levelWidth,
                          levelHeight, 0, destFormat, destType, NULL);
      }
      _mesa_TexImage2D(destTarget, destLevel, internalFormat, srcImage->Width,
                       srcImage->Height, 0, destFormat, destType, NULL);
      _mesa_BindTexture(GL_TEXTURE_2D, oldTexture2D);
      _mesa_BindTexture(GL_TEXTURE_CUBE_MAP, oldTextureCube);
      _mesa_ActiveTexture(oldActiveTexture);
      if (ctx->ErrorValue != GL_NO_ERROR)
         return;
      xoffset = 0;
      yoffset = 0;
      x = 0;
      y = 0;
      width = srcImage->Width;
      height = srcImage->Height;
   }

   dstImage = chromium_copy_texture_image(ctx, dstObj, destTarget, destLevel,
                                          func, "destination");
   if (!dstImage)
      return;
   if (!full_copy && !chromium_copy_valid_source_format(dstImage->_BaseFormat)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(destination format)", func);
      return;
   }
   if (!chromium_copy_check_bounds(ctx, srcImage, dstImage, xoffset, yoffset,
                                   x, y, width, height, func))
      return;
   if (width == 0 || height == 0)
      return;

   if (destInteger) {
      if (srcObj->Target == GL_TEXTURE_EXTERNAL_OES) {
         if (unpackFlipY || unpackPremultiplyAlpha || unpackUnmultiplyAlpha) {
            _mesa_error(ctx, GL_INVALID_OPERATION, "%s(integer transform)",
                        func);
            return;
         }
         chromium_copy_draw(ctx, srcObj->Target, sourceId, sourceLevel,
                            destTarget, destId, destLevel, xoffset, yoffset,
                            x, y, width, height, srcImage->Width,
                            srcImage->Height, GL_FALSE, GL_FALSE, GL_FALSE,
                            true);
         return;
      }
      if (!chromium_copy_valid_source_format(dstImage->_BaseFormat) ||
          srcImage->InternalFormat != dstImage->InternalFormat ||
          unpackFlipY || unpackPremultiplyAlpha || unpackUnmultiplyAlpha) {
         _mesa_error(ctx, GL_INVALID_OPERATION, "%s(integer copy)", func);
         return;
      }
      st_CopyImageSubData(ctx, srcImage, NULL, x, y, 0, dstImage, NULL,
                          xoffset, yoffset, 0, width, height, 1);
      return;
   }

   if (full_copy && chromium_copy_needs_temp_dest(internalFormat, destLevel)) {
      chromium_copy_draw_via_temp(ctx, srcObj->Target, sourceId, sourceLevel,
                                  destTarget, destId, destLevel, xoffset,
                                  yoffset, x, y, width, height,
                                  srcImage->Width, srcImage->Height,
                                  unpackFlipY, unpackPremultiplyAlpha,
                                  unpackUnmultiplyAlpha);
      return;
   }

   chromium_copy_draw(ctx, srcObj->Target, sourceId, sourceLevel,
                      destTarget, destId,
                      destLevel, xoffset, yoffset, x, y, width, height,
                      srcImage->Width, srcImage->Height, unpackFlipY,
                      unpackPremultiplyAlpha, unpackUnmultiplyAlpha, false);
}

void GLAPIENTRY
_mesa_CopyTextureCHROMIUM(GLuint sourceId, GLint sourceLevel,
                          GLenum destTarget, GLuint destId, GLint destLevel,
                          GLenum internalFormat, GLenum destType,
                          GLboolean unpackFlipY,
                          GLboolean unpackPremultiplyAlpha,
                          GLboolean unpackUnmultiplyAlpha)
{
   chromium_copy_texture_common(sourceId, sourceLevel, destTarget, destId,
                                destLevel, internalFormat, destType, 0, 0,
                                0, 0, 0, 0, unpackFlipY,
                                unpackPremultiplyAlpha, unpackUnmultiplyAlpha,
                                true, "glCopyTextureCHROMIUM");
}

void GLAPIENTRY
_mesa_CopySubTextureCHROMIUM(GLuint sourceId, GLint sourceLevel,
                             GLenum destTarget, GLuint destId,
                             GLint destLevel, GLint xoffset, GLint yoffset,
                             GLint x, GLint y, GLsizei width, GLsizei height,
                             GLboolean unpackFlipY,
                             GLboolean unpackPremultiplyAlpha,
                             GLboolean unpackUnmultiplyAlpha)
{
   chromium_copy_texture_common(sourceId, sourceLevel, destTarget, destId,
                                destLevel, 0, GL_UNSIGNED_BYTE, xoffset,
                                yoffset, x, y, width, height, unpackFlipY,
                                unpackPremultiplyAlpha, unpackUnmultiplyAlpha,
                                false, "glCopySubTextureCHROMIUM");
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
   GLsizei count;
   if (length && _mesa_get_uniform_component_count(ctx, program, location,
                                                   &count))
      *length = count;
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
   GLsizei count;
   if (length && _mesa_get_uniform_component_count(ctx, program, location,
                                                   &count))
      *length = count;
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
   GLsizei count;
   if (length && _mesa_get_uniform_component_count(ctx, program, location,
                                                   &count))
      *length = count;
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
