// SPDX-License-Identifier: Apache-2.0
/*
 * virtio_gpu_nv_main.c — module init/exit and virtio device lifecycle
 *
 * Registers a virtio driver for VIRTIO_ID_GPU_NV (0x8042).
 * On probe, allocates virtqueues and registers the /dev/nvidia* char devices.
 * On remove, tears everything down in reverse order.
 */

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "virtio_gpu_nv.h"
#include "virtio_gpu_nv_priv.h"

/* -------------------------------------------------------------------------
 * Module metadata
 * ---------------------------------------------------------------------- */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("virtio-gpu-nv contributors");
MODULE_DESCRIPTION("virtio NVIDIA ioctl forwarding driver");
MODULE_VERSION("0.1.0");

static dev_t nv_devt_base;
static struct class *nv_class;

/* One nv_dev per virtio device (i.e. per VM).  For Phase 1, only one. */
static struct nv_dev *g_nv_dev;

/* -------------------------------------------------------------------------
 * File operations (implemented in the other .c files)
 * ---------------------------------------------------------------------- */

extern const struct file_operations nv_fops;

/* -------------------------------------------------------------------------
 * /proc/driver/nvidia/ — fake procfs entries
 *
 * NVIDIA userspace libraries (libGLX_nvidia, libcuda, etc.) read
 * /proc/driver/nvidia/params during initialization. If it's missing,
 * the libraries refuse to initialize and can't export Vulkan/GL/CUDA
 * entry points.
 *
 * We provide minimal static content that satisfies the userspace check.
 * The params values are taken from a reference host and should work for
 * most configurations. The version string must match the userspace
 * library version exactly.
 * ---------------------------------------------------------------------- */

static struct proc_dir_entry *nv_proc_driver;
static struct proc_dir_entry *nv_proc_nvidia;
static struct proc_dir_entry *nv_proc_gpus;
static struct proc_dir_entry *nv_proc_gpu0;

/* params — module parameters that userspace reads during init */
static int nv_proc_params_show(struct seq_file *m, void *v)
{
  seq_puts(m,
           "ResmanDebugLevel: 4294967295\n"
           "RmLogonRC: 1\n"
           "ModifyDeviceFiles: 1\n"
           "DeviceFileUID: 0\n"
           "DeviceFileGID: 0\n"
           "DeviceFileMode: 438\n"
           "InitializeSystemMemoryAllocations: 0\n"
           "UsePageAttributeTable: 1\n"
           "EnableMSI: 1\n"
           "EnablePCIeGen3: 0\n"
           "MemoryPoolSize: 0\n"
           "KMallocHeapMaxSize: 0\n"
           "VMallocHeapMaxSize: 0\n"
           "IgnoreMMIOCheck: 0\n"
           "EnableStreamMemOPs: 0\n"
           "EnableUserNUMAManagement: 1\n"
           "NvLinkDisable: 0\n"
           "RmProfilingAdminOnly: 1\n"
           "PreserveVideoMemoryAllocations: 2\n"
           "UseKernelSuspendNotifiers: 1\n"
           "EnableS0ixPowerManagement: 1\n"
           "S0ixPowerManagementVideoMemoryThreshold: 256\n"
           "DynamicPowerManagement: 2\n"
           "DynamicPowerManagementVideoMemoryThreshold: 200\n"
           "TegraGpuPgMask: 4294967295\n"
           "RegisterPCIDriver: 1\n"
           "EnablePCIERelaxedOrderingMode: 0\n"
           "EnableResizableBar: 0\n"
           "EnableGpuFirmware: 18\n"
           "EnableGpuFirmwareLogs: 2\n"
           "RmNvlinkBandwidthLinkCount: 0\n"
           "EnableDbgBreakpoint: 0\n"
           "OpenRmEnableUnsupportedGpus: 1\n"
           "DmaRemapPeerMmio: 1\n"
           "ImexChannelCount: 2048\n"
           "CreateImexChannel0: 0\n"
           "GrdmaPciTopoCheckOverride: 0\n"
           "EnableSystemMemoryPools: 529\n"
           "CoherentGPUMemoryMode: \"\"\n"
           "RegistryDwords: \"\"\n"
           "RegistryDwordsPerDevice: \"\"\n"
           "RmMsg: \"\"\n"
           "GpuBlacklist: \"\"\n"
           "TemporaryFilePath: \"/var/tmp\"\n"
           "ExcludedGpus: \"\"\n"
  );
  return 0;
}

/* version — driver version string, must match userspace exactly */
static int nv_proc_version_show(struct seq_file *m, void *v)
{
  seq_puts(m,
           "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  "
           NV_DRIVER_VERSION
           "  Release Build  (virtio-gpu-nv@localhost)\n"
           "GCC version:  Selected multilib: .;@m64\n"
  );
  return 0;
}

/* registry — empty but must exist */
static int nv_proc_registry_show(struct seq_file *m, void *v)
{
  seq_puts(m, "Binary: \"\"\n");
  return 0;
}

/* gpus/0000:00:00.0/information — minimal GPU info */
static int nv_proc_gpu_info_show(struct seq_file *m, void *v)
{
  seq_puts(m,
           "Model:           NVIDIA Virtual GPU\n"
           "IRQ:             0\n"
           "GPU UUID:        GPU-00000000-0000-0000-0000-000000000000\n"
           "Video BIOS:      00.00.00.00.00\n"
           "Bus Type:        virtio\n"
           "DMA Size:        64 bits\n"
           "DMA Mask:        0xffffffffffffffffff\n"
           "Bus Location:    0000:00:00.0\n"
           "Device Minor:    0\n"
           "GPU Excluded:    No\n"
  );
  return 0;
}

/* gpus/0000:00:00.0/power — minimal power info */
static int nv_proc_gpu_power_show(struct seq_file *m, void *v)
{
  seq_puts(m,
           "Runtime D3 status:          Disabled\n"
           "Video Memory:               Active\n"
  );
  return 0;
}

static int nv_proc_create(void)
{
  /* /proc/driver/nvidia/ */
  nv_proc_nvidia = proc_mkdir("driver/nvidia", NULL);
  if (!nv_proc_nvidia)
    return -ENOMEM;

  /* /proc/driver/nvidia/params */
  if (!proc_create_single("params", 0444, nv_proc_nvidia,
    nv_proc_params_show))
    goto err;

  /* /proc/driver/nvidia/version */
  if (!proc_create_single("version", 0444, nv_proc_nvidia,
    nv_proc_version_show))
    goto err;

  /* /proc/driver/nvidia/registry */
  if (!proc_create_single("registry", 0644, nv_proc_nvidia,
    nv_proc_registry_show))
    goto err;

  /* /proc/driver/nvidia/gpus/0000:00:00.0/ */
  nv_proc_gpus = proc_mkdir("gpus", nv_proc_nvidia);
  if (!nv_proc_gpus)
    goto err;

  nv_proc_gpu0 = proc_mkdir("0000:00:00.0", nv_proc_gpus);
  if (!nv_proc_gpu0)
    goto err;

  if (!proc_create_single("information", 0444, nv_proc_gpu0,
    nv_proc_gpu_info_show))
    goto err;

  if (!proc_create_single("power", 0444, nv_proc_gpu0,
    nv_proc_gpu_power_show))
    goto err;

  return 0;

err:
  proc_remove(nv_proc_nvidia);
  nv_proc_nvidia = NULL;
  return -ENOMEM;
}

static void nv_proc_destroy(void)
{
  if (nv_proc_nvidia) {
    proc_remove(nv_proc_nvidia);
    nv_proc_nvidia = NULL;
  }
}

/* -------------------------------------------------------------------------
 * virtio probe
 * ---------------------------------------------------------------------- */

static int nv_probe(struct virtio_device *vdev) {
  struct nv_dev *ndev;
  struct virtqueue *vqs[NUM_QUEUES];
  struct virtqueue_info vqs_info[NUM_QUEUES] = {
      { .name = "request", .callback = nv_vq_callback },
  };
  int i, ret;

  ndev = kzalloc(sizeof(*ndev), GFP_KERNEL);
  if (!ndev)
    return -ENOMEM;

  ndev->vdev = vdev;
  vdev->priv = ndev;

  mutex_init(&ndev->vq_lock);
  init_waitqueue_head(&ndev->resp_wq);
  atomic_set(&ndev->next_cookie, 1);

  /* Allocate virtqueues. */
  ret = virtio_find_vqs(vdev, NUM_QUEUES, vqs, vqs_info, NULL);
  if (ret) {
    dev_err(&vdev->dev, "virtio_find_vqs failed: %d\n", ret);
    goto err_free;
  }
  ndev->vq = vqs[VQ_REQUEST];

  /* Allocate the shared request/response buffer.
   * We use a single bounce buffer guarded by vq_lock.
   * Phase 3 will switch to per-request allocations for concurrency. */
  ndev->buf = kmalloc(NV_BUF_SIZE, GFP_KERNEL);
  if (!ndev->buf) {
    ret = -ENOMEM;
    goto err_del_vqs;
  }

  /* Register char devices. */
  ndev->cdevs = kcalloc(NUM_MINORS, sizeof(*ndev->cdevs), GFP_KERNEL);
  if (!ndev->cdevs) {
    ret = -ENOMEM;
    goto err_free_buf;
  }

  for (i = 0; i < NUM_MINORS; i++) {
    const char *name;
    char namebuf[32];
    dev_t devt = MKDEV(MAJOR(nv_devt_base), MINOR(nv_devt_base) + i);

    cdev_init(&ndev->cdevs[i].cdev, &nv_fops);
    ndev->cdevs[i].cdev.owner = THIS_MODULE;
    ndev->cdevs[i].minor = i;
    ndev->cdevs[i].ndev = ndev;

    ret = cdev_add(&ndev->cdevs[i].cdev, devt, 1);
    if (ret) {
      dev_err(&vdev->dev, "cdev_add minor=%d failed\n", i);
      goto err_del_cdevs;
    }

    if (i == MINOR_CTL) {
      name = "nvidiactl";
    } else if (i == MINOR_UVM) {
      name = "nvidia-uvm";
    } else {
      snprintf(namebuf, sizeof(namebuf), "nvidia%d", i - MINOR_GPU_BASE);
      name = namebuf;
    }

    ndev->cdevs[i].device =
        device_create(nv_class, &vdev->dev, devt, NULL, "%s", name);
    if (IS_ERR(ndev->cdevs[i].device)) {
      ret = PTR_ERR(ndev->cdevs[i].device);
      cdev_del(&ndev->cdevs[i].cdev);
      goto err_del_cdevs;
    }
  }
  ndev->num_cdevs = NUM_MINORS;

  virtio_device_ready(vdev);
  g_nv_dev = ndev;

  dev_info(&vdev->dev, "virtio-gpu-nv: probed, %d devices registered\n",
           NUM_MINORS);
  return 0;

err_del_cdevs:
  for (i--; i >= 0; i--) {
    device_destroy(nv_class,
                   MKDEV(MAJOR(nv_devt_base), MINOR(nv_devt_base) + i));
    cdev_del(&ndev->cdevs[i].cdev);
  }
  kfree(ndev->cdevs);
err_free_buf:
  kfree(ndev->buf);
err_del_vqs:
  vdev->config->del_vqs(vdev);
err_free:
  kfree(ndev);
  return ret;
}

/* -------------------------------------------------------------------------
 * virtio remove
 * ---------------------------------------------------------------------- */

static void nv_remove(struct virtio_device *vdev) {
  struct nv_dev *ndev = vdev->priv;
  int i;

  /* Stop the device from producing more completions. */
  virtio_reset_device(vdev);

  for (i = 0; i < ndev->num_cdevs; i++) {
    device_destroy(nv_class,
                   MKDEV(MAJOR(nv_devt_base), MINOR(nv_devt_base) + i));
    cdev_del(&ndev->cdevs[i].cdev);
  }

  kfree(ndev->cdevs);
  kfree(ndev->buf);
  vdev->config->del_vqs(vdev);
  g_nv_dev = NULL;
  kfree(ndev);

  dev_info(&vdev->dev, "virtio-gpu-nv: removed\n");
}

/* -------------------------------------------------------------------------
 * virtio driver registration
 * ---------------------------------------------------------------------- */

static const struct virtio_device_id nv_id_table[] = {
    {VIRTIO_ID_GPU_NV, VIRTIO_DEV_ANY_ID},
    {0},
};
MODULE_DEVICE_TABLE(virtio, nv_id_table);

static struct virtio_driver nv_virtio_driver = {
    .driver.name = KBUILD_MODNAME,
    .driver.owner = THIS_MODULE,
    .id_table = nv_id_table,
    .probe = nv_probe,
    .remove = nv_remove,
};

/* -------------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------------- */

static char *gpu_nv_devnode(const struct device *dev, umode_t *mode)
{
  if (mode)
    *mode = 0666;
  return NULL;
}

static int __init nv_init(void) {
  int ret;

  ret = nv_proc_create();
  if (ret) {
    pr_err("virtio-gpu-nv: proc creation failed: %d\n", ret);
    goto err_proc;
  }

  ret = alloc_chrdev_region(&nv_devt_base, 0, NUM_MINORS, "nvidia");
  if (ret) {
    pr_err("virtio-gpu-nv: alloc_chrdev_region failed: %d\n", ret);
    goto err_proc;
  }

  nv_class = class_create("nvidia");
  if (IS_ERR(nv_class)) {
    ret = PTR_ERR(nv_class);
    pr_err("virtio-gpu-nv: class_create failed: %d\n", ret);
    goto err_unregister;
  }
  nv_class->devnode = gpu_nv_devnode;

  ret = register_virtio_driver(&nv_virtio_driver);
  if (ret) {
    pr_err("virtio-gpu-nv: register_virtio_driver failed: %d\n", ret);
    goto err_class;
  }

  pr_info("virtio-gpu-nv: module loaded (driver version %s)\n", NV_DRIVER_VERSION);
  return 0;

err_class:
  class_destroy(nv_class);
err_unregister:
  unregister_chrdev_region(nv_devt_base, NUM_MINORS);
err_proc:
  nv_proc_destroy();
  return ret;
}

static void __exit nv_exit(void) {
  unregister_virtio_driver(&nv_virtio_driver);
  class_destroy(nv_class);
  unregister_chrdev_region(nv_devt_base, NUM_MINORS);
  nv_proc_destroy();
  pr_info("virtio-gpu-nv: module unloaded\n");
}

module_init(nv_init);
module_exit(nv_exit);
