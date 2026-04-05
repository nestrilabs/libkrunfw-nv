// SPDX-License-Identifier: MIT
/*
 * nvidia_drm_stub.c — Minimal DRM stub that identifies as "nvidia-drm"
 *
 * The NVIDIA Vulkan ICD probes /dev/dri/renderD* via DRM_IOCTL_VERSION.
 * If none returns name="nvidia-drm", the library refuses to provide Vulkan.
 *
 * This stub registers a platform DRM device that reports name="nvidia-drm"
 * and handles DRM_NVIDIA_GET_DEV_INFO to return GPU identification data.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#define DRIVER_NAME     "nvidia-drm"
#define DRIVER_DESC     "NVIDIA DRM stub for virtio-gpu-nv"
#define DRIVER_DATE     "20250101"

/* -------------------------------------------------------------------------
 * NVIDIA custom DRM ioctl definitions
 * From kernel-open/nvidia-drm/nv_drm_common_ioctl.h
 * ---------------------------------------------------------------------- */

#define DRM_NVIDIA_GET_DEV_INFO         0x03
#define DRM_NVIDIA_FENCE_SUPPORTED      0x04
#define DRM_NVIDIA_DMABUF_SUPPORTED     0x0f

#define DRM_IOCTL_NVIDIA_GET_DEV_INFO \
    DRM_IOWR((DRM_COMMAND_BASE + DRM_NVIDIA_GET_DEV_INFO), \
             struct drm_nvidia_get_dev_info_params)

#define DRM_IOCTL_NVIDIA_FENCE_SUPPORTED \
    DRM_IO(DRM_COMMAND_BASE + DRM_NVIDIA_FENCE_SUPPORTED)

#define DRM_IOCTL_NVIDIA_DMABUF_SUPPORTED \
    DRM_IO(DRM_COMMAND_BASE + DRM_NVIDIA_DMABUF_SUPPORTED)

struct drm_nvidia_get_dev_info_params {
    uint32_t gpu_id;
    uint32_t mig_device;
    uint32_t primary_index;
    uint32_t supports_alloc;
    uint32_t generic_page_kind;
    uint32_t page_kind_generation;
    uint32_t sector_layout;
    uint32_t supports_sync_fd;
    uint32_t supports_semsurf;
};

/* -------------------------------------------------------------------------
 * DRM ioctl handlers
 * ---------------------------------------------------------------------- */

static int nv_stub_get_dev_info(struct drm_device *dev, void *data,
                                struct drm_file *filep)
{
    struct drm_nvidia_get_dev_info_params *params = data;
    /* TODO: Query from backend. Hardcoded for RTX 2060. */
    params->gpu_id              = 0x800;
    params->mig_device          = 0;
    params->primary_index       = dev->primary ? dev->primary->index : 1;
    params->supports_alloc      = 1;
    params->generic_page_kind   = 6;
    params->page_kind_generation = 2;
    params->sector_layout       = 1;
    params->supports_sync_fd    = 1;
    params->supports_semsurf    = 1;
    return 0;
}

static int nv_stub_fence_supported(struct drm_device *dev, void *data,
                                   struct drm_file *filep)
{
    return 0;
}

static int nv_stub_dmabuf_supported(struct drm_device *dev, void *data,
                                    struct drm_file *filep)
{
    return 0;
}

/* -------------------------------------------------------------------------
 * DRM ioctl table
 * ---------------------------------------------------------------------- */

static const struct drm_ioctl_desc nv_stub_ioctls[0x10] = {
    [DRM_NVIDIA_GET_DEV_INFO] = {
        .cmd = DRM_IOCTL_NVIDIA_GET_DEV_INFO,
        .func = nv_stub_get_dev_info,
        .flags = DRM_RENDER_ALLOW,
        .name = "NVIDIA_GET_DEV_INFO",
    },
    [DRM_NVIDIA_FENCE_SUPPORTED] = {
        .cmd = DRM_IOCTL_NVIDIA_FENCE_SUPPORTED,
        .func = nv_stub_fence_supported,
        .flags = DRM_RENDER_ALLOW,
        .name = "NVIDIA_FENCE_SUPPORTED",
    },
    [DRM_NVIDIA_DMABUF_SUPPORTED] = {
        .cmd = DRM_IOCTL_NVIDIA_DMABUF_SUPPORTED,
        .func = nv_stub_dmabuf_supported,
        .flags = DRM_RENDER_ALLOW,
        .name = "NVIDIA_DMABUF_SUPPORTED",
    },
};

/* -------------------------------------------------------------------------
 * DRM driver
 * ---------------------------------------------------------------------- */

static int nv_stub_open(struct drm_device *dev, struct drm_file *file)
{
    return 0;
}

static void nv_stub_postclose(struct drm_device *dev, struct drm_file *file)
{
}

static const struct file_operations nv_drm_stub_fops = {
    .owner          = THIS_MODULE,
    .open           = drm_open,
    .release        = drm_release,
    .unlocked_ioctl = drm_ioctl,
    .compat_ioctl   = drm_compat_ioctl,
    .poll           = drm_poll,
    .read           = drm_read,
    .fop_flags      = FOP_UNSIGNED_OFFSET,
};

static const struct drm_driver nv_drm_stub_driver = {
    .driver_features    = DRIVER_RENDER,
    .open               = nv_stub_open,
    .postclose          = nv_stub_postclose,
    .ioctls             = nv_stub_ioctls,
    .num_ioctls         = ARRAY_SIZE(nv_stub_ioctls),
    .fops               = &nv_drm_stub_fops,
    .name               = DRIVER_NAME,
    .desc               = DRIVER_DESC,
    .date               = DRIVER_DATE,
    .major              = 0,
    .minor              = 0,
    .patchlevel         = 0,
};

static struct platform_device *stub_pdev;
static struct drm_device *stub_drm;

static int __init nv_drm_stub_init(void)
{
    int ret;

    stub_pdev = platform_device_register_simple("nvidia-drm-stub", -1,
                                                NULL, 0);
    if (IS_ERR(stub_pdev))
        return PTR_ERR(stub_pdev);

    stub_drm = drm_dev_alloc(&nv_drm_stub_driver, &stub_pdev->dev);
    if (IS_ERR(stub_drm)) {
        ret = PTR_ERR(stub_drm);
        goto err_pdev;
    }

    ret = drm_dev_register(stub_drm, 0);
    if (ret)
        goto err_drm;

    pr_info("nvidia-drm-stub: registered as card%d / renderD%d\n",
            stub_drm->primary ? stub_drm->primary->index : -1,
            stub_drm->render ? stub_drm->render->index : -1);

    return 0;

    err_drm:
    drm_dev_put(stub_drm);
    err_pdev:
    platform_device_unregister(stub_pdev);
    return ret;
}

static void __exit nv_drm_stub_exit(void)
{
    drm_dev_unregister(stub_drm);
    drm_dev_put(stub_drm);
    platform_device_unregister(stub_pdev);
}

module_init(nv_drm_stub_init);
module_exit(nv_drm_stub_exit);

MODULE_AUTHOR("Nestri");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("MIT");
