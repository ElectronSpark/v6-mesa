/*
 * Minimal ANGLE/CHROMIUM GLES entrypoints used by Chromium's accelerated
 * command buffer loader on xv6/virgl.
 */

#include <stdbool.h>
#include <string.h>

#include "api_exec_decl.h"
#include "context.h"
#include "errors.h"
#include "macros.h"
#include "mtypes.h"
#include "texobj.h"

static bool
valid_buf_size(struct gl_context *ctx, const char *func, GLsizei bufSize)
{
   if (bufSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(bufSize=%d)", func, bufSize);
      return false;
   }

   return true;
}

static void
set_length_zero(GLsizei *length)
{
   if (length)
      *length = 0;
}

static bool
name_is_already_enabled(const char *name)
{
   static const char *const enabled_names[] = {
      "GL_ANGLE_client_arrays",
      "GL_ANGLE_pack_reverse_row_order",
      "GL_ANGLE_request_extension",
      "GL_ANGLE_robust_client_memory",
      "GL_ANGLE_texture_compression_dxt3",
      "GL_ANGLE_texture_compression_dxt5",
      "GL_ANGLE_webgl_compatibility",
      "GL_CHROMIUM_bind_generates_resource",
      "GL_CHROMIUM_copy_texture",
      "GL_KHR_debug",
   };

   for (unsigned i = 0; i < ARRAY_SIZE(enabled_names); ++i) {
      if (strcmp(name, enabled_names[i]) == 0)
         return true;
   }

   return false;
}

void GLAPIENTRY
_mesa_RequestExtensionANGLE(const GLchar *name)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!ctx)
      return;

   if (!name) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glRequestExtensionANGLE(name)");
      return;
   }

   if (name_is_already_enabled(name))
      return;

   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glRequestExtensionANGLE(%s)", name);
}

static struct gl_texture_image *
base_texture_image(struct gl_context *ctx, const struct gl_texture_object *tex,
                   const char *func)
{
   if (!tex || !tex->Image[0][0]) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "%s(texture has no level 0 image)", func);
      return NULL;
   }

   return tex->Image[0][0];
}

static GLenum
base_format_for_internal_format(GLint internalformat)
{
   switch (internalformat) {
   case GL_ALPHA:
   case GL_LUMINANCE:
   case GL_LUMINANCE_ALPHA:
   case GL_RED:
   case GL_RG:
   case GL_RGB:
   case GL_RGBA:
      return (GLenum) internalformat;
   case GL_R8:
      return GL_RED;
   case GL_RG8:
      return GL_RG;
   case GL_RGB8:
   case GL_SRGB8:
      return GL_RGB;
   case GL_RGBA8:
   case GL_SRGB8_ALPHA8:
      return GL_RGBA;
   default:
      return GL_RGBA;
   }
}

static bool
reject_copy_transform(struct gl_context *ctx, const char *func,
                      GLboolean unpack_flip_y,
                      GLboolean unpack_premultiply_alpha,
                      GLboolean unpack_unmultiply_alpha)
{
   if (unpack_flip_y || unpack_premultiply_alpha ||
       unpack_unmultiply_alpha) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "%s(transforming copy is not supported)", func);
      return false;
   }

   return true;
}

static GLsizei
image_copy_depth(const struct gl_texture_image *image)
{
   return image->Depth2 > 0 ? image->Depth2 : 1;
}

static bool
ensure_dest_2d_storage(struct gl_context *ctx, const struct gl_texture_object *dst,
                       GLuint dest_id, GLint internalformat, GLenum dest_type,
                       GLsizei width, GLsizei height)
{
   if (dst->Image[0][0])
      return true;

   if (dst->Target != GL_TEXTURE_2D) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glCopyTextureCHROMIUM(destination has no storage)");
      return false;
   }

   _mesa_TextureImage2DEXT(dest_id, GL_TEXTURE_2D, 0, internalformat,
                           width, height, 0,
                           base_format_for_internal_format(internalformat),
                           dest_type, NULL);
   return dst->Image[0][0] != NULL;
}

void GLAPIENTRY
_mesa_CopyTextureCHROMIUM(GLuint source_id, GLuint dest_id,
                          GLint internalformat, GLenum dest_type,
                          GLboolean unpack_flip_y,
                          GLboolean unpack_premultiply_alpha,
                          GLboolean unpack_unmultiply_alpha)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!ctx)
      return;

   if (!reject_copy_transform(ctx, "glCopyTextureCHROMIUM", unpack_flip_y,
                              unpack_premultiply_alpha,
                              unpack_unmultiply_alpha))
      return;

   struct gl_texture_object *src =
      _mesa_lookup_texture_err(ctx, source_id, "glCopyTextureCHROMIUM");
   struct gl_texture_object *dst =
      _mesa_lookup_texture_err(ctx, dest_id, "glCopyTextureCHROMIUM");

   if (!src || !dst)
      return;

   struct gl_texture_image *src_image =
      base_texture_image(ctx, src, "glCopyTextureCHROMIUM");
   if (!src_image)
      return;

   if (!ensure_dest_2d_storage(ctx, dst, dest_id, internalformat, dest_type,
                               src_image->Width2, src_image->Height2))
      return;

   struct gl_texture_image *dst_image =
      base_texture_image(ctx, dst, "glCopyTextureCHROMIUM");
   if (!dst_image)
      return;

   _mesa_CopyImageSubData(source_id, src->Target, 0, 0, 0, 0,
                          dest_id, dst->Target, 0, 0, 0, 0,
                          MIN2(src_image->Width2, dst_image->Width2),
                          MIN2(src_image->Height2, dst_image->Height2),
                          MIN2(image_copy_depth(src_image),
                               image_copy_depth(dst_image)));
}

void GLAPIENTRY
_mesa_CopySubTextureCHROMIUM(GLuint source_id, GLuint dest_id,
                             GLint xoffset, GLint yoffset,
                             GLint x, GLint y,
                             GLsizei width, GLsizei height,
                             GLboolean unpack_flip_y,
                             GLboolean unpack_premultiply_alpha,
                             GLboolean unpack_unmultiply_alpha)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!ctx)
      return;

   if (!reject_copy_transform(ctx, "glCopySubTextureCHROMIUM", unpack_flip_y,
                              unpack_premultiply_alpha,
                              unpack_unmultiply_alpha))
      return;

   struct gl_texture_object *src =
      _mesa_lookup_texture_err(ctx, source_id, "glCopySubTextureCHROMIUM");
   struct gl_texture_object *dst =
      _mesa_lookup_texture_err(ctx, dest_id, "glCopySubTextureCHROMIUM");

   if (!src || !dst)
      return;

   if (!base_texture_image(ctx, src, "glCopySubTextureCHROMIUM") ||
       !base_texture_image(ctx, dst, "glCopySubTextureCHROMIUM"))
      return;

   _mesa_CopyImageSubData(source_id, src->Target, 0, x, y, 0,
                          dest_id, dst->Target, 0, xoffset, yoffset, 0,
                          width, height, 1);
}

void GLAPIENTRY
_mesa_CompressedCopyTextureCHROMIUM(GLuint source_id, GLuint dest_id)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!ctx)
      return;

   struct gl_texture_object *src =
      _mesa_lookup_texture_err(ctx, source_id,
                               "glCompressedCopyTextureCHROMIUM");
   struct gl_texture_object *dst =
      _mesa_lookup_texture_err(ctx, dest_id,
                               "glCompressedCopyTextureCHROMIUM");

   if (!src || !dst)
      return;

   struct gl_texture_image *src_image =
      base_texture_image(ctx, src, "glCompressedCopyTextureCHROMIUM");
   struct gl_texture_image *dst_image =
      base_texture_image(ctx, dst, "glCompressedCopyTextureCHROMIUM");

   if (!src_image || !dst_image)
      return;

   _mesa_CopyImageSubData(source_id, src->Target, 0, 0, 0, 0,
                          dest_id, dst->Target, 0, 0, 0, 0,
                          MIN2(src_image->Width2, dst_image->Width2),
                          MIN2(src_image->Height2, dst_image->Height2),
                          MIN2(image_copy_depth(src_image),
                               image_copy_depth(dst_image)));
}

#define ROBUST_BEGIN(func)                    \
   GET_CURRENT_CONTEXT(ctx);                  \
   set_length_zero(length);                   \
   if (!ctx || !valid_buf_size(ctx, func, bufSize)) \
      return

void GLAPIENTRY
_mesa_GetBooleanvRobustANGLE(GLenum pname, GLsizei bufSize,
                             GLsizei *length, GLboolean *data)
{
   ROBUST_BEGIN("glGetBooleanvRobustANGLE");
   _mesa_GetBooleanv(pname, data);
}

void GLAPIENTRY
_mesa_GetBufferParameteri64vRobustANGLE(GLenum target, GLenum pname,
                                        GLsizei bufSize, GLsizei *length,
                                        GLint64 *params)
{
   ROBUST_BEGIN("glGetBufferParameteri64vRobustANGLE");
   _mesa_GetBufferParameteri64v(target, pname, params);
}

void GLAPIENTRY
_mesa_GetBufferParameterivRobustANGLE(GLenum target, GLenum pname,
                                      GLsizei bufSize, GLsizei *length,
                                      GLint *params)
{
   ROBUST_BEGIN("glGetBufferParameterivRobustANGLE");
   _mesa_GetBufferParameteriv(target, pname, params);
}

void GLAPIENTRY
_mesa_GetBufferPointervRobustANGLE(GLenum target, GLenum pname,
                                   GLsizei bufSize, GLsizei *length,
                                   GLvoid **params)
{
   ROBUST_BEGIN("glGetBufferPointervRobustANGLE");
   _mesa_GetBufferPointerv(target, pname, params);
}

void GLAPIENTRY
_mesa_GetFloatvRobustANGLE(GLenum pname, GLsizei bufSize,
                           GLsizei *length, GLfloat *data)
{
   ROBUST_BEGIN("glGetFloatvRobustANGLE");
   _mesa_GetFloatv(pname, data);
}

void GLAPIENTRY
_mesa_GetFramebufferAttachmentParameterivRobustANGLE(GLenum target,
                                                     GLenum attachment,
                                                     GLenum pname,
                                                     GLsizei bufSize,
                                                     GLsizei *length,
                                                     GLint *params)
{
   ROBUST_BEGIN("glGetFramebufferAttachmentParameterivRobustANGLE");
   _mesa_GetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}

static void
unsupported_pixel_local_storage(const char *func, GLsizei *length)
{
   GET_CURRENT_CONTEXT(ctx);

   set_length_zero(length);
   if (ctx)
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(extension not supported)",
                  func);
}

void GLAPIENTRY
_mesa_GetFramebufferPixelLocalStorageParameterfvRobustANGLE(GLint plane,
                                                            GLenum pname,
                                                            GLsizei bufSize,
                                                            GLsizei *length,
                                                            GLfloat *params)
{
   (void) plane;
   (void) pname;
   (void) bufSize;
   (void) params;
   unsupported_pixel_local_storage(
      "glGetFramebufferPixelLocalStorageParameterfvRobustANGLE", length);
}

void GLAPIENTRY
_mesa_GetFramebufferPixelLocalStorageParameterivRobustANGLE(GLint plane,
                                                            GLenum pname,
                                                            GLsizei bufSize,
                                                            GLsizei *length,
                                                            GLint *params)
{
   (void) plane;
   (void) pname;
   (void) bufSize;
   (void) params;
   unsupported_pixel_local_storage(
      "glGetFramebufferPixelLocalStorageParameterivRobustANGLE", length);
}

void GLAPIENTRY
_mesa_GetFramebufferPixelLocalStorageParameteruivRobustANGLE(GLint plane,
                                                             GLenum pname,
                                                             GLsizei bufSize,
                                                             GLsizei *length,
                                                             GLuint *params)
{
   (void) plane;
   (void) pname;
   (void) bufSize;
   (void) params;
   unsupported_pixel_local_storage(
      "glGetFramebufferPixelLocalStorageParameteruivRobustANGLE", length);
}

void GLAPIENTRY
_mesa_GetInteger64i_vRobustANGLE(GLenum target, GLuint index,
                                 GLsizei bufSize, GLsizei *length,
                                 GLint64 *data)
{
   ROBUST_BEGIN("glGetInteger64i_vRobustANGLE");
   _mesa_GetInteger64i_v(target, index, data);
}

void GLAPIENTRY
_mesa_GetInteger64vRobustANGLE(GLenum pname, GLsizei bufSize,
                               GLsizei *length, GLint64 *data)
{
   ROBUST_BEGIN("glGetInteger64vRobustANGLE");
   _mesa_GetInteger64v(pname, data);
}

void GLAPIENTRY
_mesa_GetIntegeri_vRobustANGLE(GLenum target, GLuint index,
                               GLsizei bufSize, GLsizei *length,
                               GLint *data)
{
   ROBUST_BEGIN("glGetIntegeri_vRobustANGLE");
   _mesa_GetIntegeri_v(target, index, data);
}

void GLAPIENTRY
_mesa_GetIntegervRobustANGLE(GLenum pname, GLsizei bufSize,
                             GLsizei *length, GLint *data)
{
   ROBUST_BEGIN("glGetIntegervRobustANGLE");
   _mesa_GetIntegerv(pname, data);
}

void GLAPIENTRY
_mesa_GetInternalformativRobustANGLE(GLenum target, GLenum internalformat,
                                     GLenum pname, GLsizei bufSize,
                                     GLsizei *length, GLint *params)
{
   ROBUST_BEGIN("glGetInternalformativRobustANGLE");
   _mesa_GetInternalformativ(target, internalformat, pname, bufSize, params);
}

void GLAPIENTRY
_mesa_GetMultisamplefvRobustANGLE(GLenum pname, GLuint index,
                                  GLsizei bufSize, GLsizei *length,
                                  GLfloat *val)
{
   ROBUST_BEGIN("glGetMultisamplefvRobustANGLE");
   _mesa_GetMultisamplefv(pname, index, val);
}

void GLAPIENTRY
_mesa_GetProgramivRobustANGLE(GLuint program, GLenum pname,
                              GLsizei bufSize, GLsizei *length,
                              GLint *params)
{
   ROBUST_BEGIN("glGetProgramivRobustANGLE");
   _mesa_GetProgramiv(program, pname, params);
}

void GLAPIENTRY
_mesa_GetQueryivRobustANGLE(GLenum target, GLenum pname,
                            GLsizei bufSize, GLsizei *length,
                            GLint *params)
{
   ROBUST_BEGIN("glGetQueryivRobustANGLE");
   _mesa_GetQueryiv(target, pname, params);
}

void GLAPIENTRY
_mesa_GetQueryObjecti64vRobustANGLE(GLuint id, GLenum pname,
                                    GLsizei bufSize, GLsizei *length,
                                    GLint64 *params)
{
   ROBUST_BEGIN("glGetQueryObjecti64vRobustANGLE");
   _mesa_GetQueryObjecti64v(id, pname, params);
}

void GLAPIENTRY
_mesa_GetQueryObjectivRobustANGLE(GLuint id, GLenum pname,
                                  GLsizei bufSize, GLsizei *length,
                                  GLint *params)
{
   ROBUST_BEGIN("glGetQueryObjectivRobustANGLE");
   _mesa_GetQueryObjectiv(id, pname, params);
}

void GLAPIENTRY
_mesa_GetQueryObjectui64vRobustANGLE(GLuint id, GLenum pname,
                                     GLsizei bufSize, GLsizei *length,
                                     GLuint64 *params)
{
   ROBUST_BEGIN("glGetQueryObjectui64vRobustANGLE");
   _mesa_GetQueryObjectui64v(id, pname, params);
}

void GLAPIENTRY
_mesa_GetQueryObjectuivRobustANGLE(GLuint id, GLenum pname,
                                   GLsizei bufSize, GLsizei *length,
                                   GLuint *params)
{
   ROBUST_BEGIN("glGetQueryObjectuivRobustANGLE");
   _mesa_GetQueryObjectuiv(id, pname, params);
}

void GLAPIENTRY
_mesa_GetRenderbufferParameterivRobustANGLE(GLenum target, GLenum pname,
                                            GLsizei bufSize,
                                            GLsizei *length,
                                            GLint *params)
{
   ROBUST_BEGIN("glGetRenderbufferParameterivRobustANGLE");
   _mesa_GetRenderbufferParameteriv(target, pname, params);
}

void GLAPIENTRY
_mesa_GetSamplerParameterfvRobustANGLE(GLuint sampler, GLenum pname,
                                       GLsizei bufSize, GLsizei *length,
                                       GLfloat *params)
{
   ROBUST_BEGIN("glGetSamplerParameterfvRobustANGLE");
   _mesa_GetSamplerParameterfv(sampler, pname, params);
}

void GLAPIENTRY
_mesa_GetSamplerParameterivRobustANGLE(GLuint sampler, GLenum pname,
                                       GLsizei bufSize, GLsizei *length,
                                       GLint *params)
{
   ROBUST_BEGIN("glGetSamplerParameterivRobustANGLE");
   _mesa_GetSamplerParameteriv(sampler, pname, params);
}

void GLAPIENTRY
_mesa_GetShaderivRobustANGLE(GLuint shader, GLenum pname,
                             GLsizei bufSize, GLsizei *length,
                             GLint *params)
{
   ROBUST_BEGIN("glGetShaderivRobustANGLE");
   _mesa_GetShaderiv(shader, pname, params);
}

void GLAPIENTRY
_mesa_GetTexLevelParameterfvRobustANGLE(GLenum target, GLint level,
                                        GLenum pname, GLsizei bufSize,
                                        GLsizei *length, GLfloat *params)
{
   ROBUST_BEGIN("glGetTexLevelParameterfvRobustANGLE");
   _mesa_GetTexLevelParameterfv(target, level, pname, params);
}

void GLAPIENTRY
_mesa_GetTexLevelParameterivRobustANGLE(GLenum target, GLint level,
                                        GLenum pname, GLsizei bufSize,
                                        GLsizei *length, GLint *params)
{
   ROBUST_BEGIN("glGetTexLevelParameterivRobustANGLE");
   _mesa_GetTexLevelParameteriv(target, level, pname, params);
}

void GLAPIENTRY
_mesa_GetTexParameterfvRobustANGLE(GLenum target, GLenum pname,
                                   GLsizei bufSize, GLsizei *length,
                                   GLfloat *params)
{
   ROBUST_BEGIN("glGetTexParameterfvRobustANGLE");
   _mesa_GetTexParameterfv(target, pname, params);
}

void GLAPIENTRY
_mesa_GetTexParameterivRobustANGLE(GLenum target, GLenum pname,
                                   GLsizei bufSize, GLsizei *length,
                                   GLint *params)
{
   ROBUST_BEGIN("glGetTexParameterivRobustANGLE");
   _mesa_GetTexParameteriv(target, pname, params);
}

void GLAPIENTRY
_mesa_GetUniformfvRobustANGLE(GLuint program, GLint location,
                              GLsizei bufSize, GLsizei *length,
                              GLfloat *params)
{
   ROBUST_BEGIN("glGetUniformfvRobustANGLE");
   _mesa_GetnUniformfvARB(program, location, bufSize, params);
}

void GLAPIENTRY
_mesa_GetUniformivRobustANGLE(GLuint program, GLint location,
                              GLsizei bufSize, GLsizei *length,
                              GLint *params)
{
   ROBUST_BEGIN("glGetUniformivRobustANGLE");
   _mesa_GetnUniformivARB(program, location, bufSize, params);
}

void GLAPIENTRY
_mesa_GetUniformuivRobustANGLE(GLuint program, GLint location,
                               GLsizei bufSize, GLsizei *length,
                               GLuint *params)
{
   ROBUST_BEGIN("glGetUniformuivRobustANGLE");
   _mesa_GetnUniformuivARB(program, location, bufSize, params);
}

void GLAPIENTRY
_mesa_GetVertexAttribIivRobustANGLE(GLuint index, GLenum pname,
                                    GLsizei bufSize, GLsizei *length,
                                    GLint *params)
{
   ROBUST_BEGIN("glGetVertexAttribIivRobustANGLE");
   _mesa_GetVertexAttribIiv(index, pname, params);
}

void GLAPIENTRY
_mesa_GetVertexAttribIuivRobustANGLE(GLuint index, GLenum pname,
                                     GLsizei bufSize, GLsizei *length,
                                     GLuint *params)
{
   ROBUST_BEGIN("glGetVertexAttribIuivRobustANGLE");
   _mesa_GetVertexAttribIuiv(index, pname, params);
}

void GLAPIENTRY
_mesa_GetVertexAttribPointervRobustANGLE(GLuint index, GLenum pname,
                                         GLsizei bufSize, GLsizei *length,
                                         GLvoid **pointer)
{
   ROBUST_BEGIN("glGetVertexAttribPointervRobustANGLE");
   _mesa_GetVertexAttribPointerv(index, pname, pointer);
}

void GLAPIENTRY
_mesa_GetVertexAttribfvRobustANGLE(GLuint index, GLenum pname,
                                   GLsizei bufSize, GLsizei *length,
                                   GLfloat *params)
{
   ROBUST_BEGIN("glGetVertexAttribfvRobustANGLE");
   _mesa_GetVertexAttribfv(index, pname, params);
}

void GLAPIENTRY
_mesa_GetVertexAttribivRobustANGLE(GLuint index, GLenum pname,
                                   GLsizei bufSize, GLsizei *length,
                                   GLint *params)
{
   ROBUST_BEGIN("glGetVertexAttribivRobustANGLE");
   _mesa_GetVertexAttribiv(index, pname, params);
}

void GLAPIENTRY
_mesa_ReadPixelsRobustANGLE(GLint x, GLint y, GLsizei width, GLsizei height,
                            GLenum format, GLenum type, GLsizei bufSize,
                            GLsizei *length, GLsizei *columns,
                            GLsizei *rows, GLvoid *pixels)
{
   GET_CURRENT_CONTEXT(ctx);

   set_length_zero(length);
   if (columns)
      *columns = 0;
   if (rows)
      *rows = 0;

   if (!ctx || !valid_buf_size(ctx, "glReadPixelsRobustANGLE", bufSize))
      return;

   _mesa_ReadnPixelsARB(x, y, width, height, format, type, bufSize, pixels);

   if (columns)
      *columns = width;
   if (rows)
      *rows = height;
}

#define ROBUST_INPUT_BEGIN(func)              \
   GET_CURRENT_CONTEXT(ctx);                  \
   if (!ctx || !valid_buf_size(ctx, func, bufSize)) \
      return

void GLAPIENTRY
_mesa_SamplerParameterfvRobustANGLE(GLuint sampler, GLenum pname,
                                    GLsizei bufSize, const GLfloat *param)
{
   ROBUST_INPUT_BEGIN("glSamplerParameterfvRobustANGLE");
   _mesa_SamplerParameterfv(sampler, pname, param);
}

void GLAPIENTRY
_mesa_SamplerParameterivRobustANGLE(GLuint sampler, GLenum pname,
                                    GLsizei bufSize, const GLint *param)
{
   ROBUST_INPUT_BEGIN("glSamplerParameterivRobustANGLE");
   _mesa_SamplerParameteriv(sampler, pname, param);
}

void GLAPIENTRY
_mesa_TexImage2DRobustANGLE(GLenum target, GLint level, GLint internalformat,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, GLsizei bufSize,
                            const GLvoid *pixels)
{
   ROBUST_INPUT_BEGIN("glTexImage2DRobustANGLE");
   _mesa_TexImage2D(target, level, internalformat, width, height, border,
                    format, type, pixels);
}

void GLAPIENTRY
_mesa_TexImage3DRobustANGLE(GLenum target, GLint level, GLint internalformat,
                            GLsizei width, GLsizei height, GLsizei depth,
                            GLint border, GLenum format, GLenum type,
                            GLsizei bufSize, const GLvoid *pixels)
{
   ROBUST_INPUT_BEGIN("glTexImage3DRobustANGLE");
   _mesa_TexImage3D(target, level, internalformat, width, height, depth,
                    border, format, type, pixels);
}

void GLAPIENTRY
_mesa_TexParameterfvRobustANGLE(GLenum target, GLenum pname,
                                GLsizei bufSize, const GLfloat *params)
{
   ROBUST_INPUT_BEGIN("glTexParameterfvRobustANGLE");
   _mesa_TexParameterfv(target, pname, params);
}

void GLAPIENTRY
_mesa_TexParameterivRobustANGLE(GLenum target, GLenum pname,
                                GLsizei bufSize, const GLint *params)
{
   ROBUST_INPUT_BEGIN("glTexParameterivRobustANGLE");
   _mesa_TexParameteriv(target, pname, params);
}

void GLAPIENTRY
_mesa_TexSubImage2DRobustANGLE(GLenum target, GLint level,
                               GLint xoffset, GLint yoffset,
                               GLsizei width, GLsizei height,
                               GLenum format, GLenum type, GLsizei bufSize,
                               const GLvoid *pixels)
{
   ROBUST_INPUT_BEGIN("glTexSubImage2DRobustANGLE");
   _mesa_TexSubImage2D(target, level, xoffset, yoffset, width, height,
                       format, type, pixels);
}

void GLAPIENTRY
_mesa_TexSubImage3DRobustANGLE(GLenum target, GLint level,
                               GLint xoffset, GLint yoffset, GLint zoffset,
                               GLsizei width, GLsizei height, GLsizei depth,
                               GLenum format, GLenum type, GLsizei bufSize,
                               const GLvoid *pixels)
{
   ROBUST_INPUT_BEGIN("glTexSubImage3DRobustANGLE");
   _mesa_TexSubImage3D(target, level, xoffset, yoffset, zoffset, width,
                       height, depth, format, type, pixels);
}

#undef ROBUST_INPUT_BEGIN
#undef ROBUST_BEGIN
