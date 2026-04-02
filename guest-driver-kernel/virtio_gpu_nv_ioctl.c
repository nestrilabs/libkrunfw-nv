// SPDX-License-Identifier: Apache-2.0
/*
 * virtio_gpu_nv_ioctl.c — file operations: open, release, ioctl
 *
 * open():
 *   Sends NV_MSG_OPEN to the backend, receives a guest_handle, stores it
 *   in file->private_data.
 *
 * release():
 *   Sends NV_MSG_CLOSE to the backend.
 *
 * ioctl():
 *   Copies the raw ioctl param bytes from userspace, sends NV_MSG_IOCTL,
 *   waits for the response, copies updated bytes back.
 *
 * --- On "NOT ABI-aware" ---
 *
 * The guest driver does not parse or interpret any NVIDIA ioctl struct
 * fields.  All semantic logic (handle translation, struct layout,
 * nested-dispatch) lives in the backend.
 *
 * However, the guest driver does use _IOC_SIZE(cmd) to determine how many
 * bytes to copy_from_user / copy_to_user.  This means the ioctl numbers
 * in the user-mode library must encode the correct size for the driver
 * version running on the HOST — because the host backend is the one that
 * actually interprets those bytes.
 *
 * In practice this is always true: the user-mode NVIDIA libraries are
 * version-locked to the kernel driver, and both run at the host's version.
 * The guest's copy of libcuda/libvulkan_nvidia is the same binary as on
 * the host (or the same version), so _IOC_SIZE values match.
 *
 * If you ever load a different-version user-mode library in the guest, the
 * backend will catch the mismatch via NV_ESC_CHECK_VERSION_STR and return
 * an error before any struct is misinterpreted.  We do NOT need to validate
 * _IOC_SIZE in the guest driver beyond the NV_MAX_PARAM_SIZE safety cap.
 *
 * mmap() is in virtio_gpu_nv_mmap.c.
 */

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/file.h>

#include "virtio_gpu_nv.h"
#include "virtio_gpu_nv_priv.h"

/* -------------------------------------------------------------------------
 * Cookie generation
 * ---------------------------------------------------------------------- */

static u64 next_cookie(struct nv_dev *ndev) {
  return (u64)atomic_inc_return(&ndev->next_cookie);
}

/* -------------------------------------------------------------------------
 * nv_open — called when userspace opens /dev/nvidia*
 * ---------------------------------------------------------------------- */

static int nv_open(struct inode *inode, struct file *filp) {
  struct nv_cdev *ncdev = container_of(inode->i_cdev, struct nv_cdev, cdev);
  struct nv_dev *ndev = ncdev->ndev;
  struct nv_file_ctx *ctx;
  struct msg_header req_hdr;
  struct open_req req_payload;
  struct nv_request resp;
  struct open_resp *oresp;
  int ret;

  resp.resp_payload = NULL;

  ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
  if (!ctx)
    return -ENOMEM;

  ctx->dev = ndev;
  INIT_LIST_HEAD(&ctx->mappings);
  spin_lock_init(&ctx->mappings_lock);

  memset(&req_payload, 0, sizeof(req_payload));
  req_payload.kind = ncdev->kind;
  req_payload.index = ncdev->gpu_index;

  req_hdr.msg_type = cpu_to_le32(NV_MSG_OPEN);
  req_hdr.cookie = cpu_to_le64(next_cookie(ndev));
  req_hdr._pad = 0;

  ret = nv_do_request(ndev, &req_hdr, sizeof(req_hdr), &req_payload,
                      sizeof(req_payload), &resp);
  if (ret)
    goto err_free;

  if (le32_to_cpu(resp.resp_hdr.status) != NV_STATUS_OK) {
    pr_err("nv_open: backend returned status %u (host errno %d)\n",
           le32_to_cpu(resp.resp_hdr.status),
           le32_to_cpu(resp.resp_hdr.errno_host));
    ret = -EIO;
    goto err_free;
  }

  if (resp.resp_payload_len < sizeof(struct open_resp)) {
    pr_err("nv_open: response payload too short\n");
    ret = -EIO;
    goto err_free;
  }

  oresp = (struct open_resp *)resp.resp_payload;
  ctx->guest_handle = le64_to_cpu(oresp->guest_handle);
  kfree(resp.resp_payload);
  filp->private_data = ctx;
  return 0;

err_free:
  kfree(resp.resp_payload);
  kfree(ctx);
  return ret;
}

/* -------------------------------------------------------------------------
 * nv_release — called when the last fd reference is dropped
 * ---------------------------------------------------------------------- */

static int nv_release(struct inode *inode, struct file *filp) {
  struct nv_file_ctx *ctx = filp->private_data;
  struct nv_dev *ndev = ctx->dev;
  struct msg_header req_hdr;
  struct close_req req_payload;
  struct nv_request resp;
  int ret;

  resp.resp_payload = NULL;

  req_hdr.msg_type = cpu_to_le32(NV_MSG_CLOSE);
  req_hdr.cookie = cpu_to_le64(next_cookie(ndev));
  req_hdr._pad = 0;

  req_payload.guest_handle = cpu_to_le64(ctx->guest_handle);

  ret = nv_do_request(ndev, &req_hdr, sizeof(req_hdr), &req_payload,
                      sizeof(req_payload), &resp);
  if (ret)
    pr_warn("nv_release: nv_do_request failed: %d\n", ret);
  else if (le32_to_cpu(resp.resp_hdr.status) != NV_STATUS_OK)
    pr_warn("nv_release: backend status %u\n",
            le32_to_cpu(resp.resp_hdr.status));

  /* Free any recorded mapping info. */
  {
    struct nv_mapping_info *mi, *tmp;
    spin_lock(&ctx->mappings_lock);
    list_for_each_entry_safe(mi, tmp, &ctx->mappings, list) {
      list_del(&mi->list);
      kfree(mi);
    }
    spin_unlock(&ctx->mappings_lock);
  }

  kfree(resp.resp_payload);
  kfree(ctx);
  filp->private_data = NULL;
  return 0;
}

/* -------------------------------------------------------------------------
 * NVOS54_PARAMETERS layout (NV_ESC_RM_CONTROL, escape 0x2A)
 *   offset  0: hClient       u32
 *   offset  4: hObject       u32
 *   offset  8: cmd           u32
 *   offset 12: flags         u32
 *   offset 16: params        u64  ← USERSPACE POINTER
 *   offset 24: paramsSize    u32
 *   offset 28: status        u32
 *   total: 32 bytes
 *
 * NVOS64_PARAMETERS layout (NV_ESC_RM_ALLOC, escape 0x2B)
 *   offset  0: hRoot         u32
 *   offset  4: hObjectParent u32
 *   offset  8: hObjectNew    u32
 *   offset 12: hClass        u32
 *   offset 16: pAllocParms   u64  ← USERSPACE POINTER
 *   offset 24: pRightsReq    u64  ← USERSPACE POINTER (handled as NULL for now)
 *   offset 32: paramsSize    u32
 *   offset 36: flags         u32
 *   offset 40: status        u32
 *   offset 44: _pad          u32
 *   total: 48 bytes
 * ---------------------------------------------------------------------- */

#define RMCTL_OUTER_SIZE       32
#define RMCTL_PTR_OFFSET       16
#define RMCTL_SIZE_OFFSET      24

#define RMALLOC_OUTER_SIZE     48
#define RMALLOC_PTR_OFFSET     16
#define RMALLOC_SIZE_OFFSET    32

/*
 * nv_ioctl_nested — handle ioctls with embedded userspace pointers.
 *
 * Copies both the outer struct and the nested param buffer from guest
 * userspace, sends them concatenated (outer + nested) to the backend.
 * On response, copies both back to guest userspace.
 */
static long nv_ioctl_nested(struct file *filp, unsigned int cmd,
                            unsigned long arg, unsigned int outer_size,
                            unsigned int ptr_offset,
                            unsigned int size_offset)
{
  struct nv_file_ctx *ctx = filp->private_data;
  struct nv_dev *ndev = ctx->dev;
  void __user *uarg = (void __user *)arg;

  u8 outer[48]; /* big enough for both NVOS54 and NVOS64 */
  u64 user_ptr;
  u32 nested_size;
  void *nested_buf = NULL;
  void *combined = NULL;
  u32 total_size;

  struct {
    struct msg_header req_hdr;
    struct ioctl_req ioctl_hdr;
  } req;
  struct nv_request resp;
  struct ioctl_resp *iresp;
  long ret;

  resp.resp_payload = NULL;

  if (outer_size > sizeof(outer))
    return -EINVAL;

  if (copy_from_user(outer, uarg, outer_size))
    return -EFAULT;

  /* Extract embedded pointer and nested param size */
  memcpy(&user_ptr, &outer[ptr_offset], sizeof(u64));
  memcpy(&nested_size, &outer[size_offset], sizeof(u32));

  /* Copy nested params from guest userspace if present */
  if (user_ptr && nested_size > 0) {
    if (nested_size > NV_MAX_PARAM_SIZE - outer_size)
      return -EINVAL;

    nested_buf = kmalloc(nested_size, GFP_KERNEL);
    if (!nested_buf)
      return -ENOMEM;

    if (copy_from_user(nested_buf, (void __user *)user_ptr, nested_size)) {
      ret = -EFAULT;
      goto out;
    }
  }

  /* Zero the pointer — backend will set its own host pointer */
  memset(&outer[ptr_offset], 0, sizeof(u64));

  /* Build combined buffer: outer + nested */
  total_size = outer_size + (nested_buf ? nested_size : 0);
  combined = kmalloc(total_size, GFP_KERNEL);
  if (!combined) {
    ret = -ENOMEM;
    goto out;
  }

  memcpy(combined, outer, outer_size);
  if (nested_buf)
    memcpy(combined + outer_size, nested_buf, nested_size);

  /* Send request */
  req.req_hdr.msg_type = cpu_to_le32(NV_MSG_IOCTL);
  req.req_hdr.cookie = cpu_to_le64(next_cookie(ndev));
  req.req_hdr._pad = 0;

  req.ioctl_hdr.guest_handle = cpu_to_le64(ctx->guest_handle);
  req.ioctl_hdr.request = cpu_to_le64((u64)cmd);
  req.ioctl_hdr.param_size = cpu_to_le32(total_size);
  req.ioctl_hdr._pad = 0;

  ret = nv_do_request(ndev, &req, sizeof(req), combined, total_size, &resp);
  if (ret)
    goto out;

  if (le32_to_cpu(resp.resp_hdr.status) != NV_STATUS_OK) {
    int host_errno = le32_to_cpu(resp.resp_hdr.errno_host);
    ret = host_errno ? -host_errno : -EIO;
    goto out;
  }

  if (resp.resp_payload_len < sizeof(struct ioctl_resp)) {
    ret = -EIO;
    goto out;
  }

  iresp = (struct ioctl_resp *)resp.resp_payload;

  /* Copy results back to guest userspace */
  {
    void *resp_params = (char *)resp.resp_payload + sizeof(struct ioctl_resp);
    u32 resp_total = le32_to_cpu(iresp->param_size);

    if (resp_total >= outer_size) {
      /* Restore original userspace pointer before copying outer back */
      memcpy(resp_params + ptr_offset, &user_ptr, sizeof(u64));

      if (copy_to_user(uarg, resp_params, outer_size)) {
        ret = -EFAULT;
        goto out;
      }

      /* Copy nested params back to original userspace pointer */
      if (user_ptr && nested_size > 0) {
        u32 resp_nested = resp_total - outer_size;
        u32 copy_len = min_t(u32, resp_nested, nested_size);
        if (copy_len > 0) {
          if (copy_to_user((void __user *)user_ptr,
                           resp_params + outer_size, copy_len)) {
            ret = -EFAULT;
            goto out;
          }
        }
      }
    }
  }

  ret = 0;
out:
  kfree(resp.resp_payload);
  kfree(combined);
  kfree(nested_buf);
  return ret;
}

/* -------------------------------------------------------------------------
 * guest_fd_to_handle — translate a guest fd to its backend handle
 *
 * Used for fd-carrying ioctls where userspace embeds a raw fd number
 * referring to another /dev/nvidia* device.
 * ---------------------------------------------------------------------- */

extern const struct file_operations nv_fops;

static int guest_fd_to_handle(int guest_fd, u64 *out_handle)
{
  struct file *f;
  struct nv_file_ctx *ctx;

  f = fget(guest_fd);
  if (!f)
    return -EBADF;

  /* Verify it's one of our devices */
  if (f->f_op != &nv_fops) {
    fput(f);
    return -EINVAL;
  }

  ctx = f->private_data;
  *out_handle = ctx->guest_handle;
  fput(f);
  return 0;
}

/* -------------------------------------------------------------------------
 * nv_ioctl_fd_carrying — handle ioctls with embedded fd numbers
 *
 * NV_ESC_REGISTER_FD:   fd at offset 0 (4 bytes total)
 * NV_ESC_ALLOC_OS_EVENT: fd at offset 8 (16 bytes total)
 * NV_ESC_FREE_OS_EVENT:  fd at offset 8 (16 bytes total)
 * ---------------------------------------------------------------------- */

static long nv_ioctl_fd_carrying(struct file *filp, unsigned int cmd,
                                 unsigned long arg, unsigned int fd_offset)
{
  struct nv_file_ctx *ctx = filp->private_data;
  struct nv_dev *ndev = ctx->dev;
  unsigned int param_size = _IOC_SIZE(cmd);
  void __user *uarg = (void __user *)arg;

  struct {
    struct msg_header req_hdr;
    struct ioctl_req ioctl_hdr;
  } req;

  struct nv_request resp;
  struct ioctl_resp *iresp;
  void *param_buf = NULL;
  int orig_fd;
  u64 handle;
  long ret;

  resp.resp_payload = NULL;

  if (param_size > NV_MAX_PARAM_SIZE || param_size < fd_offset + 4)
    return -EINVAL;

  param_buf = kmalloc(param_size, GFP_KERNEL);
  if (!param_buf)
    return -ENOMEM;

  if (copy_from_user(param_buf, uarg, param_size)) {
    ret = -EFAULT;
    goto out;
  }

  /* Read the embedded fd number */
  memcpy(&orig_fd, param_buf + fd_offset, sizeof(int));

  /* Translate fd → guest_handle */
  ret = guest_fd_to_handle(orig_fd, &handle);
  if (ret) {
    pr_err("nv_ioctl_fd_carrying: bad embedded fd %d: %ld\n", orig_fd, ret);
    goto out;
  }

  /* Replace fd with guest_handle (backend will translate to host fd) */
  {
    u32 handle32 = (u32)handle;
    memcpy(param_buf + fd_offset, &handle32, sizeof(u32));
  }

  /* Send request */
  req.req_hdr.msg_type = cpu_to_le32(NV_MSG_IOCTL);
  req.req_hdr.cookie = cpu_to_le64(next_cookie(ndev));
  req.req_hdr._pad = 0;

  req.ioctl_hdr.guest_handle = cpu_to_le64(ctx->guest_handle);
  req.ioctl_hdr.request = cpu_to_le64((u64)cmd);
  req.ioctl_hdr.param_size = cpu_to_le32(param_size);
  req.ioctl_hdr._pad = 0;

  ret = nv_do_request(ndev, &req, sizeof(req), param_buf, param_size, &resp);
  if (ret)
    goto out;

  if (le32_to_cpu(resp.resp_hdr.status) != NV_STATUS_OK) {
    int host_errno = le32_to_cpu(resp.resp_hdr.errno_host);
    ret = host_errno ? -host_errno : -EIO;
    goto out;
  }

  if (resp.resp_payload_len < sizeof(struct ioctl_resp)) {
    ret = -EIO;
    goto out;
  }

  iresp = (struct ioctl_resp *)resp.resp_payload;

  if (param_size && iresp->param_size) {
    u32 copy_len = min_t(u32, param_size, le32_to_cpu(iresp->param_size));
    void *resp_params = (char *)resp.resp_payload + sizeof(struct ioctl_resp);

    /* Restore original fd before copying back to userspace */
    if (copy_len >= fd_offset + 4)
      memcpy(resp_params + fd_offset, &orig_fd, sizeof(int));

    if (copy_to_user(uarg, resp_params, copy_len)) {
      ret = -EFAULT;
      goto out;
    }
  }

  ret = 0;
out:
  kfree(resp.resp_payload);
  kfree(param_buf);
  return ret;
}

/* -------------------------------------------------------------------------
 * nv_ioctl — main dispatch
 * ---------------------------------------------------------------------- */

long nv_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
  unsigned int escape = _IOC_NR(cmd);

  /* Ioctls with embedded userspace pointers need special handling */
  switch (escape) {
    case 0x2A: /* NV_ESC_RM_CONTROL */
      return nv_ioctl_nested(filp, cmd, arg,
                             RMCTL_OUTER_SIZE, RMCTL_PTR_OFFSET, RMCTL_SIZE_OFFSET);
    case 0x2B: /* NV_ESC_RM_ALLOC */
      return nv_ioctl_nested(filp, cmd, arg,
                             RMALLOC_OUTER_SIZE, RMALLOC_PTR_OFFSET, RMALLOC_SIZE_OFFSET);
    case 0xC9: /* NV_ESC_REGISTER_FD */
      return nv_ioctl_fd_carrying(filp, cmd, arg, 0);
    case 0xCE: /* NV_ESC_ALLOC_OS_EVENT */
      return nv_ioctl_fd_carrying(filp, cmd, arg, 8);
    case 0xCF: /* NV_ESC_FREE_OS_EVENT */
      return nv_ioctl_fd_carrying(filp, cmd, arg, 8);
    default:
      break;
  }

  /* Original path for simple ioctls (no embedded pointers) */
  {
    struct nv_file_ctx *ctx = filp->private_data;
    struct nv_dev *ndev = ctx->dev;
    unsigned int param_size = _IOC_SIZE(cmd);

    struct {
      struct msg_header req_hdr;
      struct ioctl_req ioctl_hdr;
    } req;

    struct nv_request resp;
    struct ioctl_resp *iresp;
    void *param_buf = NULL;
    long ret;

    resp.resp_payload = NULL;

    if (param_size > NV_MAX_PARAM_SIZE)
      return -EINVAL;

    if (param_size) {
      param_buf = kmalloc(param_size, GFP_KERNEL);
      if (!param_buf)
        return -ENOMEM;

      if (copy_from_user(param_buf, (void __user *)arg, param_size)) {
        ret = -EFAULT;
        goto out_simple;
      }
    }

    req.req_hdr.msg_type = cpu_to_le32(NV_MSG_IOCTL);
    req.req_hdr.cookie = cpu_to_le64(next_cookie(ndev));
    req.req_hdr._pad = 0;

    req.ioctl_hdr.guest_handle = cpu_to_le64(ctx->guest_handle);
    req.ioctl_hdr.request = cpu_to_le64((u64)cmd);
    req.ioctl_hdr.param_size = cpu_to_le32(param_size);
    req.ioctl_hdr._pad = 0;

    ret = nv_do_request(ndev, &req, sizeof(req), param_buf, param_size, &resp);
    if (ret)
      goto out_simple;

    if (le32_to_cpu(resp.resp_hdr.status) != NV_STATUS_OK) {
      int host_errno = le32_to_cpu(resp.resp_hdr.errno_host);
      ret = host_errno ? -host_errno : -EIO;
      goto out_simple;
    }

    if (resp.resp_payload_len < sizeof(struct ioctl_resp)) {
      ret = -EIO;
      goto out_simple;
    }

    iresp = (struct ioctl_resp *)resp.resp_payload;

    if (param_size && iresp->param_size) {
      u32 copy_len = min_t(u32, param_size, le32_to_cpu(iresp->param_size));
      void *resp_params = (char *)resp.resp_payload + sizeof(struct ioctl_resp);

      if (copy_to_user((void __user *)arg, resp_params, copy_len)) {
        ret = -EFAULT;
        goto out_simple;
      }
    }

    if (le64_to_cpu(iresp->shm_length) > 0) {
      struct nv_mapping_info *mi = kmalloc(sizeof(*mi), GFP_KERNEL);
      if (mi) {
        mi->shm_offset = le64_to_cpu(iresp->shm_offset);
        mi->shm_length = le64_to_cpu(iresp->shm_length);
        mi->pgprot = iresp->pgprot;
        spin_lock(&ctx->mappings_lock);
        list_add_tail(&mi->list, &ctx->mappings);
        spin_unlock(&ctx->mappings_lock);
      }
    }

    ret = 0;
out_simple:
    kfree(resp.resp_payload);
    kfree(param_buf);
    return ret;
  }
}

/* -------------------------------------------------------------------------
 * poll — stub for Phase 1; Phase 4 will implement GPU event polling
 * ---------------------------------------------------------------------- */

static __poll_t nv_poll(struct file *filp, struct poll_table_struct *wait) {
  pr_warn_once("nv_ioctl: nv_poll not yet implemented, fill just return EPOLLIN | EPOLLOUT");
  return EPOLLIN | EPOLLOUT;
}

/* -------------------------------------------------------------------------
 * File operations table
 * ---------------------------------------------------------------------- */

const struct file_operations nv_fops = {
    .owner = THIS_MODULE,
    .open = nv_open,
    .release = nv_release,
    .unlocked_ioctl = nv_ioctl,
    .compat_ioctl = nv_ioctl,
    .mmap = nv_mmap,
    .poll = nv_poll,
};
