// SPDX-License-Identifier: MIT
/*
 * nvidia_drm_stub.c — Minimal DRM stub that identifies as "nvidia-drm"
 *
 * Uses a platform device but synthesises the sysfs attributes that the
 * NVIDIA Vulkan ICD reads when walking up from /dev/dri/renderD* to
 * determine whether it is on a PCI discrete GPU or a Tegra SoC.
 *
 * The ICD checks for the presence and content of:
 *   <drm-node>/device/vendor          → "0x10de"
 *   <drm-node>/device/device          → "0x1f08"
 *   <drm-node>/device/class           → "0x030200"
 *   <drm-node>/device/subsystem_vendor
 *   <drm-node>/device/subsystem_device
 *
 * It also checks that <drm-node>/device is NOT under
 * /sys/bus/platform (Tegra path).  We achieve this by creating a
 * second fake "pci"-bus device as the parent of the platform device,
 * which moves the sysfs subtree out of /sys/devices/platform/.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#define DRIVER_NAME "nvidia-drm"
#define DRIVER_DESC "NVIDIA DRM stub for virtio-gpu-nv"
#define DRIVER_DATE "20250101"

/* ----------------------------------------------------------------
 * Module parameters — match these to what CARD_INFO returns
 * ---------------------------------------------------------------- */
static uint vendor_id = 0x10de;
static uint device_id = 0x1f08;
static uint subsys_id = 0x0000;
static uint pci_domain = 0x0000;
static uint pci_bus = 0x08;
static uint pci_slot = 0x00;
static uint pci_func = 0x00;

module_param(vendor_id, uint, 0444);
module_param(device_id, uint, 0444);
module_param(subsys_id, uint, 0444);
module_param(pci_domain, uint, 0444);
module_param(pci_bus, uint, 0444);
module_param(pci_slot, uint, 0444);
module_param(pci_func, uint, 0444);

MODULE_PARM_DESC(vendor_id, "PCI vendor ID  (default 0x10de = NVIDIA)");
MODULE_PARM_DESC(device_id, "PCI device ID  (default 0x1f08 = RTX 2060)");
MODULE_PARM_DESC(pci_bus, "PCI bus number shown in sysfs device name");
MODULE_PARM_DESC(pci_slot, "PCI slot number shown in sysfs device name");

/* ----------------------------------------------------------------
 * NVIDIA custom DRM ioctls
 * ---------------------------------------------------------------- */
#define DRM_NVIDIA_GET_DEV_INFO 0x03
#define DRM_NVIDIA_FENCE_SUPPORTED 0x04
#define DRM_NVIDIA_DMABUF_SUPPORTED 0x0f

#define DRM_IOCTL_NVIDIA_GET_DEV_INFO                                          \
  DRM_IOWR((DRM_COMMAND_BASE + DRM_NVIDIA_GET_DEV_INFO),                       \
           struct drm_nvidia_get_dev_info_params)
#define DRM_IOCTL_NVIDIA_FENCE_SUPPORTED                                       \
  DRM_IO(DRM_COMMAND_BASE + DRM_NVIDIA_FENCE_SUPPORTED)
#define DRM_IOCTL_NVIDIA_DMABUF_SUPPORTED                                      \
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

/* ----------------------------------------------------------------
 * DRM ioctl handlers
 * ---------------------------------------------------------------- */
static int nv_stub_get_dev_info(struct drm_device *dev, void *data,
                                struct drm_file *filep) {
  struct drm_nvidia_get_dev_info_params *p = data;
  p->gpu_id = 0x800;
  p->mig_device = 0;
  p->primary_index = dev->primary ? dev->primary->index : 1;
  p->supports_alloc = 1;
  p->generic_page_kind = 6;
  p->page_kind_generation = 2;
  p->sector_layout = 1;
  p->supports_sync_fd = 1;
  p->supports_semsurf = 1;
  return 0;
}

static int nv_stub_fence_supported(struct drm_device *dev, void *data,
                                   struct drm_file *filep) {
  return 0;
}

static int nv_stub_dmabuf_supported(struct drm_device *dev, void *data,
                                    struct drm_file *filep) {
  return 0;
}

static const struct drm_ioctl_desc nv_stub_ioctls[0x10] = {
    [DRM_NVIDIA_GET_DEV_INFO] =
        {
            .cmd = DRM_IOCTL_NVIDIA_GET_DEV_INFO,
            .func = nv_stub_get_dev_info,
            .flags = DRM_RENDER_ALLOW,
            .name = "NVIDIA_GET_DEV_INFO",
        },
    [DRM_NVIDIA_FENCE_SUPPORTED] =
        {
            .cmd = DRM_IOCTL_NVIDIA_FENCE_SUPPORTED,
            .func = nv_stub_fence_supported,
            .flags = DRM_RENDER_ALLOW,
            .name = "NVIDIA_FENCE_SUPPORTED",
        },
    [DRM_NVIDIA_DMABUF_SUPPORTED] =
        {
            .cmd = DRM_IOCTL_NVIDIA_DMABUF_SUPPORTED,
            .func = nv_stub_dmabuf_supported,
            .flags = DRM_RENDER_ALLOW,
            .name = "NVIDIA_DMABUF_SUPPORTED",
        },
};

static const struct file_operations nv_drm_stub_fops = {
    .owner = THIS_MODULE,
    .open = drm_open,
    .release = drm_release,
    .unlocked_ioctl = drm_ioctl,
    .compat_ioctl = drm_compat_ioctl,
    .poll = drm_poll,
    .read = drm_read,
    .fop_flags = FOP_UNSIGNED_OFFSET,
};

static const struct drm_driver nv_drm_stub_driver = {
    .driver_features = DRIVER_RENDER,
    .open = nv_stub_open,
    .postclose = nv_stub_postclose,
    .ioctls = nv_stub_ioctls,
    .num_ioctls = ARRAY_SIZE(nv_stub_ioctls),
    .fops = &nv_drm_stub_fops,
    .name = DRIVER_NAME,
    .desc = DRIVER_DESC,
    .date = DRIVER_DATE,
    .major = 0,
    .minor = 0,
    .patchlevel = 0,
};

/* ----------------------------------------------------------------
 * Fake PCI-bus device
 *
 * We register a plain struct device on the "pci" bus so its sysfs
 * path becomes /sys/devices/pci<domain>:<bus>/<domain>:<bus>:<slot>.<func>
 * instead of /sys/devices/platform/…
 *
 * The NVIDIA ICD checks that the parent bus is NOT "platform" to
 * decide it is on a real discrete GPU.
 * ---------------------------------------------------------------- */

/* Minimal bus type — we only need a name, no match/probe logic */
static int fake_pci_bus_match(struct device *dev,
                              const struct device_driver *drv) {
  return 0;
}

static struct bus_type fake_pci_bus = {
    .name = "pci", /* Must be "pci" — the ICD checks bus->name */
    .match = fake_pci_bus_match,
};

/*
 * sysfs attributes read by the NVIDIA ICD on the device node.
 *
 * The ICD opens these files directly:
 *   /sys/class/drm/renderDN/device/vendor
 *   /sys/class/drm/renderDN/device/device
 *   /sys/class/drm/renderDN/device/class
 *   /sys/class/drm/renderDN/device/subsystem_vendor
 *   /sys/class/drm/renderDN/device/subsystem_device
 */
#define FAKE_PCI_ATTR_SHOW(attr_name, fmt, val)                                \
  static ssize_t attr_name##_show(struct device *d,                            \
                                  struct device_attribute *a, char *buf) {     \
    return sysfs_emit(buf, fmt "\n", val);                                     \
  }                                                                            \
  static DEVICE_ATTR_RO(attr_name)

FAKE_PCI_ATTR_SHOW(vendor, "0x%04x", vendor_id);
FAKE_PCI_ATTR_SHOW(device, "0x%04x", device_id);
FAKE_PCI_ATTR_SHOW(class, "0x%06x", 0x030200);
FAKE_PCI_ATTR_SHOW(subsystem_vendor, "0x%04x", vendor_id);
FAKE_PCI_ATTR_SHOW(subsystem_device, "0x%04x", subsys_id);

static struct attribute *fake_pci_dev_attrs[] = {
    &dev_attr_vendor.attr,
    &dev_attr_device.attr,
    &dev_attr_class.attr,
    &dev_attr_subsystem_vendor.attr,
    &dev_attr_subsystem_device.attr,
    NULL,
};
static const struct attribute_group fake_pci_dev_group = {
    .attrs = fake_pci_dev_attrs,
};

static void fake_pci_dev_release(struct device *dev) {}

static struct device fake_pci_dev; /* the "GPU" device */
static struct drm_device *stub_drm;
static bool bus_registered;
static bool dev_registered;
static bool attrs_added;

/* ----------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------- */

/*
 * We need nv_stub_open and nv_stub_postclose declared before the
 * drm_driver struct — move them above it.
 */
static int nv_stub_open(struct drm_device *dev, struct drm_file *file) {
  return 0;
}

static void nv_stub_postclose(struct drm_device *dev, struct drm_file *file) {}

static int __init nv_drm_stub_init(void) {
  int ret;
  char dev_name[32];

  /*
   * Step 1: Register our fake "pci" bus.
   *
   * We cannot reuse the real pci_bus_type here because we do not
   * want to fight with the real PCI subsystem.  Our bus just needs
   * the name "pci" so the ICD's bus-name check passes.
   */
  ret = bus_register(&fake_pci_bus);
  if (ret) {
    pr_err("nvidia-drm-stub: bus_register failed: %d\n", ret);
    return ret;
  }
  bus_registered = true;

  /*
   * Step 2: Create the fake GPU device.
   *
   * Name it "<domain>:<bus>:<slot>.<func>" — exactly how the real
   * kernel PCI core names PCI devices in sysfs.
   */
  snprintf(dev_name, sizeof(dev_name), "%04x:%02x:%02x.%x", pci_domain, pci_bus,
           pci_slot, pci_func);

  device_initialize(&fake_pci_dev);
  fake_pci_dev.bus = &fake_pci_bus;
  fake_pci_dev.release = fake_pci_dev_release;

  ret = dev_set_name(&fake_pci_dev, "%s", dev_name);
  if (ret)
    goto err;

  ret = device_add(&fake_pci_dev);
  if (ret) {
    pr_err("nvidia-drm-stub: device_add failed: %d\n", ret);
    goto err;
  }
  dev_registered = true;

  /*
   * Step 3: Add the PCI sysfs attributes the ICD reads as plain files.
   */
  ret = sysfs_create_group(&fake_pci_dev.kobj, &fake_pci_dev_group);
  if (ret) {
    pr_err("nvidia-drm-stub: sysfs_create_group failed: %d\n", ret);
    goto err;
  }
  attrs_added = true;

  /*
   * Step 4: Allocate and register the DRM device, parented to our
   * fake PCI device.  This makes the renderD* node appear at:
   *   /sys/devices/pci<domain>:<bus>/<domain>:<bus>:<slot>.<func>/drm/renderD*
   * and
   *   /sys/class/drm/renderD* → .../renderD*/
      device → fake_pci_dev * /
      stub_drm = drm_dev_alloc(&nv_drm_stub_driver, &fake_pci_dev);
  if (IS_ERR(stub_drm)) {
    ret = PTR_ERR(stub_drm);
    pr_err("nvidia-drm-stub: drm_dev_alloc failed: %d\n", ret);
    goto err;
  }

  ret = drm_dev_register(stub_drm, 0);
  if (ret) {
    pr_err("nvidia-drm-stub: drm_dev_register failed: %d\n", ret);
    drm_dev_put(stub_drm);
    stub_drm = NULL;
    goto err;
  }

  pr_info("nvidia-drm-stub: registered renderD%d under %s\n",
          stub_drm->render ? stub_drm->render->index : -1, dev_name);
  return 0;

err:
  if (attrs_added)
    sysfs_remove_group(&fake_pci_dev.kobj, &fake_pci_dev_group);
  if (dev_registered)
    device_del(&fake_pci_dev);
  put_device(&fake_pci_dev);
  if (bus_registered)
    bus_unregister(&fake_pci_bus);
  return ret;
}

static void __exit nv_drm_stub_exit(void) {
  if (stub_drm) {
    drm_dev_unregister(stub_drm);
    drm_dev_put(stub_drm);
  }
  if (attrs_added)
    sysfs_remove_group(&fake_pci_dev.kobj, &fake_pci_dev_group);
  if (dev_registered)
    device_del(&fake_pci_dev);
  put_device(&fake_pci_dev);
  if (bus_registered)
    bus_unregister(&fake_pci_bus);
}

module_init(nv_drm_stub_init);
module_exit(nv_drm_stub_exit);

MODULE_AUTHOR("Nestri");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("MIT");
