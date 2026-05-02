#include "util/detect_os.h"
#include "git_sha1.h"
#include "dri_screen.h"
 
#include "target-helpers/drm_helper.h"
#include "target-helpers/sw_helper.h"

extern const __DRI2configQueryExtension dri2GalliumConfigQueryExtension;

static struct dri_context *
xv6_dri_create_context(struct dri_screen *screen, int api,
                       const struct dri_config *config,
                       struct dri_context *shared, unsigned num_attribs,
                       const uint32_t *attribs, unsigned *error,
                       void *loader_private)
{
   return driCreateContextAttribs(screen, api, config, shared, num_attribs,
                                  attribs, error, loader_private, false);
}

static struct dri_screen *
xv6_dri_create_screen3(int screen, int fd,
                       const __DRIextension **loader_extensions,
                       const __DRIextension **driver_extensions,
                       const struct dri_config ***driver_configs,
                       bool driver_name_is_inferred,
                       void *loader_private)
{
   (void)driver_extensions;
   return driCreateNewScreen3(screen, fd, loader_extensions, DRI_SCREEN_DRI3,
                              driver_configs, driver_name_is_inferred, true,
                              loader_private);
}

static const __DRImesaCoreExtension xv6_mesa_core_extension = {
   .base = { __DRI_MESA, 2 },
   .version_string = MESA_INTERFACE_VERSION_STRING,
   .createContext = xv6_dri_create_context,
   .initScreen = NULL,
   .queryCompatibleRenderOnlyDeviceFd = NULL,
   .createNewScreen3 = xv6_dri_create_screen3,
};

static const __DRIextension *xv6_dri_driver_extensions[] = {
   &xv6_mesa_core_extension.base,
   &dri2GalliumConfigQueryExtension.base,
   &gallium_config_options.base,
   NULL,
};

const __DRIextension **__driDriverGetExtensions_virtio_gpu(void);
PUBLIC const __DRIextension **__driDriverGetExtensions_virtio_gpu(void)
{
   return xv6_dri_driver_extensions;
}

const __DRIextension **__driDriverGetExtensions_swrast(void);
PUBLIC const __DRIextension **__driDriverGetExtensions_swrast(void)
{
   return xv6_dri_driver_extensions;
}

const __DRIextension **__driDriverGetExtensions_kms_swrast(void);
PUBLIC const __DRIextension **__driDriverGetExtensions_kms_swrast(void)
{
   return xv6_dri_driver_extensions;
}
