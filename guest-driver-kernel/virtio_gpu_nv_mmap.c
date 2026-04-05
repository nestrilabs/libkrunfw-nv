// SPDX-License-Identifier: Apache-2.0
/*
 * virtio_gpu_nv_mmap.c — mmap file operation
 *
 * Called by userspace after a mapping ioctl (e.g. NV_ESC_RM_MAP_MEMORY).
 * The backend has already:
 *   1. Issued the host ioctl.
 *   2. mmap'd the resulting host mapping into the SHM BAR at some offset.
 *   3. Returned (shm_offset, shm_length, pgprot) in the ioctl response.
 *
 * The NVIDIA user-mode driver then calls mmap(fd, offset=shm_offset) to
 * get the GPU mapping into its address space.  We satisfy that here by
 * calling remap_pfn_range() to map the corresponding SHM BAR PFNs.
 *
 * Phase 1: The backend never returns shm_offset != 0 (no mapping ioctls
 * yet), so this path is exercised in Phase 3.  The code is written now
 * so the infrastructure is in place.
 */

#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>

#include "virtio_gpu_nv.h"
#include "virtio_gpu_nv_priv.h"

/*
 * PFN of the first byte of the SHM BAR in the guest physical address space.
 *
 * Filled in during virtio_probe when the VMM exposes the BAR via a
 * virtio memory region or a KVM memslot.  Phase 1: set to 0 (unused).
 *
 * Phase 3 will read this from virtio config space:
 *   virtio_cread(vdev, struct nv_config, shm_bar_gpa, &ndev->shm_bar_gpa);
 */
static unsigned long g_shm_bar_pfn; /* guest PFN of SHM BAR base */

/* Called from probe once we know the GPA of the SHM BAR. */
void nv_set_shm_bar_pfn(unsigned long pfn) { g_shm_bar_pfn = pfn; }

/* -------------------------------------------------------------------------
 * nv_mmap
 * ---------------------------------------------------------------------- */

int nv_mmap(struct file *filp, struct vm_area_struct *vma) {
  struct nv_file_ctx *ctx = filp->private_data;
  struct nv_dev *ndev = ctx->dev;
  unsigned long size = vma->vm_end - vma->vm_start;
  struct nv_mapping_info *found = NULL;
  unsigned long shm_off;
  unsigned long pfn;
  pgprot_t pgprot;
  int ret;

  pr_info("nv_mmap: ENTERED handle=%llu pgoff=0x%lx size=0x%lx\n",
          ((struct nv_file_ctx *)filp->private_data)->guest_handle,
          vma->vm_pgoff, vma->vm_end - vma->vm_start);

  if (!g_shm_bar_pfn) {
    pr_warn_once("nv_mmap: SHM BAR not configured\n");
    return -ENODEV;
  }

  if (vma->vm_pgoff != 0) {
    pr_err("nv_mmap: non-zero pgoff 0x%lx rejected\n", vma->vm_pgoff);
    return -EINVAL;
  }

  /*
   * Use device-global mapping list. The most recently pushed entry
   * (tail) is the current active mmap context — like the real driver's
   * file_mapping_list which is a single-slot that RM overwrites.
   *
   * We don't consume entries here. They persist until overwritten
   * by the next RM_MAP_MEMORY or removed by RM_UNMAP_MEMORY.
   * This allows multiple mmaps of the same mapping (which the
   * real driver supports — both ctl and gpu fds can mmap the same
   * allocation).
   */
  spin_lock(&ndev->mappings_lock);
  if (!list_empty(&ndev->mappings)) {
    found = list_last_entry(&ndev->mappings, struct nv_mapping_info, list);
  }
  spin_unlock(&ndev->mappings_lock);

  if (!found) {
    pr_err("nv_mmap: no mapping context (handle=%llu size=0x%lx)\n",
           ctx->guest_handle, size);
    return -EINVAL;
  }

  shm_off = found->shm_offset;

  if (size > found->shm_length) {
    unsigned long alloc_pages = (found->shm_length + PAGE_SIZE - 1) >> PAGE_SHIFT;
    unsigned long req_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (req_pages > alloc_pages) {
      pr_err("nv_mmap: size 0x%lx exceeds alloc 0x%llx\n",
             size, found->shm_length);
      return -EINVAL;
    }
  }

  pfn = g_shm_bar_pfn + (shm_off >> PAGE_SHIFT);

  switch (found->pgprot) {
  case 0: pgprot = vma->vm_page_prot; break;
  case 1: pgprot = pgprot_writecombine(vma->vm_page_prot); break;
  case 2: pgprot = pgprot_noncached(vma->vm_page_prot); break;
  default: pgprot = pgprot_noncached(vma->vm_page_prot); break;
  }

  vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
  vma->vm_page_prot = pgprot;

  ret = remap_pfn_range(vma, vma->vm_start, pfn, size, pgprot);
  if (ret)
    pr_err("nv_mmap: remap_pfn_range failed: %d\n", ret);
  else
    pr_info("nv_mmap: mapped shm_offset=0x%lx pfn=0x%lx size=0x%lx\n",
            (unsigned long)shm_off, pfn, size);

  return ret;
}
