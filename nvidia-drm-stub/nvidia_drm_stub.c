// SPDX-License-Identifier: MIT
/*
 * nvidia_drm_stub.c — Minimal DRM stub that identifies as "nvidia-drm"
 *
 * Registers as a virtual PCI device so the NVIDIA Vulkan ICD sees the
 * correct PCI topology when walking sysfs from /dev/dri/renderD*.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#define DRIVER_NAME "nvidia-drm"
#define DRIVER_DESC "NVIDIA DRM stub for virtio-gpu-nv"
#define DRIVER_DATE "20250101"

/* ----------------------------------------------------------------
 * NVIDIA custom DRM ioctl definitions
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

/*
 * Module parameters — override these to match what your proxy
 * returns in CARD_INFO (vendor_id, device_id) and the PCI address
 * reported by the host RM (domain:bus:slot.func).
 *
 * Defaults match an RTX 2060 at 0000:08:00.0.
 */
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

MODULE_PARM_DESC(vendor_id, "PCI vendor ID (default 0x10de = NVIDIA)");
MODULE_PARM_DESC(device_id, "PCI device ID (default 0x1f08 = RTX 2060)");
MODULE_PARM_DESC(pci_bus, "PCI bus number to report in sysfs");
MODULE_PARM_DESC(pci_slot, "PCI slot number to report in sysfs");

/* ----------------------------------------------------------------
 * DRM ioctl handlers
 * ---------------------------------------------------------------- */

static int nv_stub_get_dev_info(struct drm_device *dev, void *data,
                                struct drm_file *filep) {
  struct drm_nvidia_get_dev_info_params *params = data;
  params->gpu_id = 0x800;
  params->mig_device = 0;
  params->primary_index = dev->primary ? dev->primary->index : 1;
  params->supports_alloc = 1;
  params->generic_page_kind = 6;
  params->page_kind_generation = 2;
  params->sector_layout = 1;
  params->supports_sync_fd = 1;
  params->supports_semsurf = 1;
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

/* ----------------------------------------------------------------
 * DRM ioctl table
 * ---------------------------------------------------------------- */

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

/* ----------------------------------------------------------------
 * DRM driver
 * ---------------------------------------------------------------- */

static int nv_stub_open(struct drm_device *dev, struct drm_file *file) {
  return 0;
}

static void nv_stub_postclose(struct drm_device *dev, struct drm_file *file) {}

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
 * Virtual PCI device
 *
 * We use a root bus + virtual PCI device so that sysfs shows:
 *   /sys/bus/pci/devices/0000:08:00.0/drm/renderD129
 * instead of:
 *   /sys/devices/platform/nvidia-drm-stub/drm/renderD129
 *
 * The NVIDIA Vulkan ICD walks this path to determine whether it is
 * running on a discrete PCI-e GPU or a Tegra SoC.
 * ---------------------------------------------------------------- */

static struct pci_bus *stub_bus;
static struct pci_dev *stub_pci_dev;
static struct drm_device *stub_drm;

/*
 * Fake config space.  The Vulkan ICD reads at minimum:
 *   offset 0x00 — vendor/device ID
 *   offset 0x08 — class code
 *   offset 0x2c — subsystem IDs
 */
static int stub_pci_read(struct pci_bus *bus, unsigned int devfn, int where,
                         int size, u32 *val) {
  if (PCI_SLOT(devfn) != pci_slot || PCI_FUNC(devfn) != pci_func) {
    *val = ~0U;
    return PCIBIOS_DEVICE_NOT_FOUND;
  }

  switch (where) {
  case PCI_VENDOR_ID: /* 0x00 — vendor + device */
    *val = (device_id << 16) | vendor_id;
    break;
  case PCI_COMMAND: /* 0x04 */
    *val = PCI_COMMAND_MEMORY;
    break;
  case PCI_CLASS_REVISION: /* 0x08 — class 0x0302 = 3D controller */
    *val = 0x03020000;
    break;
  case PCI_SUBSYSTEM_VENDOR_ID: /* 0x2c */
    *val = (subsys_id << 16) | vendor_id;
    break;
  default:
    *val = 0;
    break;
  }
  return PCIBIOS_SUCCESSFUL;
}

static int stub_pci_write(struct pci_bus *bus, unsigned int devfn, int where,
                          int size, u32 val) {
  return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops stub_pci_ops = {
    .read = stub_pci_read,
    .write = stub_pci_write,
};

/*
 * sysfs attributes the NVIDIA driver reads on the PCI device node.
 * Without these the ICD cannot confirm the GPU class/vendor.
 */
static ssize_t vendor_show(struct device *d, struct device_attribute *a,
                           char *buf) {
  return sysfs_emit(buf, "0x%04x\n", vendor_id);
}

static ssize_t device_show(struct device *d, struct device_attribute *a,
                           char *buf) {
  return sysfs_emit(buf, "0x%04x\n", device_id);
}

static ssize_t class_show(struct device *d, struct device_attribute *a,
                          char *buf) {
  /* 0x030200 — PCI class "3D Controller" (NVIDIA uses this) */
  return sysfs_emit(buf, "0x030200\n");
}

static ssize_t subsystem_vendor_show(struct device *d,
                                     struct device_attribute *a, char *buf) {
  return sysfs_emit(buf, "0x%04x\n", vendor_id);
}

static ssize_t subsystem_device_show(struct device *d,
                                     struct device_attribute *a, char *buf) {
  return sysfs_emit(buf, "0x%04x\n", subsys_id);
}

static DEVICE_ATTR_RO(vendor);
static DEVICE_ATTR_RO(device);
static DEVICE_ATTR_RO(class);
static DEVICE_ATTR_RO(subsystem_vendor);
static DEVICE_ATTR_RO(subsystem_device);

static struct attribute *stub_pci_attrs[] = {
    &dev_attr_vendor.attr,
    &dev_attr_device.attr,
    &dev_attr_class.attr,
    &dev_attr_subsystem_vendor.attr,
    &dev_attr_subsystem_device.attr,
    NULL,
};

static const struct attribute_group stub_pci_attr_group = {
    .attrs = stub_pci_attrs,
};

/* ----------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------- */

static int __init nv_drm_stub_init(void) {
  int ret;
  struct pci_host_bridge *bridge;

  /*
   * Step 1: Allocate a fake PCI host bridge + bus.
   * This gives us a real /sys/bus/pci/devices/XXXX:XX:XX.X entry.
   */
  bridge = pci_alloc_host_bridge(0);
  if (!bridge)
    return -ENOMEM;

  bridge->dev.parent = NULL;
  pci_set_host_bridge_release(bridge, NULL, NULL);

  stub_bus =
      pci_create_root_bus(NULL, pci_bus, &stub_pci_ops, NULL, &bridge->windows);
  if (!stub_bus) {
    ret = -ENOMEM;
    goto err_bridge;
  }

  stub_bus->domain_nr = pci_domain;

  /*
   * Step 2: Scan the fake bus — this will call stub_pci_read()
   * and materialise a struct pci_dev at slot:func.
   */
  pci_scan_child_bus(stub_bus);
  pci_bus_add_devices(stub_bus);

  /*
   * Step 3: Find the pci_dev we just created.
   */
  stub_pci_dev = pci_get_slot(stub_bus, PCI_DEVFN(pci_slot, pci_func));
  if (!stub_pci_dev) {
    pr_err("nvidia-drm-stub: pci_get_slot failed\n");
    ret = -ENODEV;
    goto err_bus;
  }

  /*
   * Step 4: Add the extra sysfs attributes (vendor, device, class…)
   * that the Vulkan ICD reads directly as files.
   */
  ret = sysfs_create_group(&stub_pci_dev->dev.kobj, &stub_pci_attr_group);
  if (ret)
    goto err_pci_dev;

  /*
   * Step 5: Allocate and register the DRM device, parented to the
   * fake PCI device so its sysfs path becomes:
   *   /sys/devices/pci<domain>:<bus>/<domain>:<bus>:<slot>.<func>/drm/renderD*
   */
  stub_drm = drm_dev_alloc(&nv_drm_stub_driver, &stub_pci_dev->dev);
  if (IS_ERR(stub_drm)) {
    ret = PTR_ERR(stub_drm);
    goto err_sysfs;
  }

  ret = drm_dev_register(stub_drm, 0);
  if (ret)
    goto err_drm;

  pr_info("nvidia-drm-stub: registered renderD%d under PCI %04x:%02x:%02x.%x\n",
          stub_drm->render ? stub_drm->render->index : -1, pci_domain, pci_bus,
          pci_slot, pci_func);

  return 0;

err_drm:
  drm_dev_put(stub_drm);
err_sysfs:
  sysfs_remove_group(&stub_pci_dev->dev.kobj, &stub_pci_attr_group);
err_pci_dev:
  pci_dev_put(stub_pci_dev);
err_bus:
  pci_remove_root_bus(stub_bus);
err_bridge:
  /* bridge is freed by pci_remove_root_bus / pci_free_host_bridge */
  return ret;
}

static void __exit nv_drm_stub_exit(void) {
  drm_dev_unregister(stub_drm);
  drm_dev_put(stub_drm);
  sysfs_remove_group(&stub_pci_dev->dev.kobj, &stub_pci_attr_group);
  pci_dev_put(stub_pci_dev);
  pci_remove_root_bus(stub_bus);
}

module_init(nv_drm_stub_init);
module_exit(nv_drm_stub_exit);

MODULE_AUTHOR("Nestri");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("MIT");
