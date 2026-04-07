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

/* NVIDIA class IDs from open-gpu-kernel-modules */
#include <class/cl0002.h>  /* NV01_CONTEXT_DMA_FROM_MEMORY */
#include <class/cl003e.h>  /* NV01_MEMORY_SYSTEM */
#include <class/cl0040.h>  /* NV01_MEMORY_LOCAL_USER */
#include <class/cl0070.h>  /* NV01_MEMORY_VIRTUAL */
#include <class/cl0080.h>  /* NV01_DEVICE_0 */
#include <class/cl2080.h>  /* NV20_SUBDEVICE_0 */
#include <class/cl2081.h>  /* NV2081_BINAPI */
#include <class/cl50a0.h>  /* NV50_MEMORY_VIRTUAL */
#include <class/cl90f1.h>  /* FERMI_VASPACE_A */
#include <class/cla06c.h>  /* KEPLER_CHANNEL_GROUP_A */

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

  kfree(resp.resp_payload);
  kfree(ctx);
  filp->private_data = NULL;
  return 0;
}

/* -------------------------------------------------------------------------
 * RM_ALLOC hClass → pAllocParms size lookup
 *
 * When paramsSize == 0 but pAllocParms is non-null, the host kernel
 * determines param size from hClass internally.  We need to know the
 * size to copy from guest userspace.  This mirrors gVisor nvproxy's
 * per-class alloc param type table.
 *
 * Classes that use rmAllocNoParams (pAllocParms should be NULL) are
 * not listed here.  The fallback handles unknown classes.
 * ---------------------------------------------------------------------- */

#define RMALLOC_HCLASS_OFFSET 12

static u32 rmalloc_class_param_size(u32 hClass)
{
    switch (hClass) {
    /* Root client - NV04_MEMORY (0x0041) */
    case 0x0041: return 12;  /* NV0000_ALLOC_PARAMETERS */

    /* Device / subdevice */
    case NV01_DEVICE_0: return 52;  /* NV0080_ALLOC_PARAMETERS */
    case NV20_SUBDEVICE_0: return  4;  /* NV2080_ALLOC_PARAMETERS */
    case NV2081_BINAPI: return  4;  /* NV2081_ALLOC_PARAMETERS */

    /* Context DMA — NV_CONTEXT_DMA_ALLOCATION_PARAMS (32 bytes) */
    case NV01_CONTEXT_DMA_FROM_MEMORY: return 32;

    /* Memory allocation classes — NV_MEMORY_ALLOCATION_PARAMS (128 bytes) */
    case NV01_MEMORY_SYSTEM: return 64;  /* NV_MEMORY_ALLOCATION_PARAMS (NV01_MEMORY_SYSTEM) */
    case NV01_MEMORY_LOCAL_USER: return 64;  /* NV_MEMORY_ALLOCATION_PARAMS (NV01_MEMORY_LOCAL_USER) */
    case NV50_MEMORY_VIRTUAL: return 64;  /* NV_MEMORY_ALLOCATION_PARAMS (NV50_MEMORY_VIRTUAL) */

    /* Memory virtual — NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS (24 bytes) */
    case NV01_MEMORY_VIRTUAL: return 24;  /* NV01_MEMORY_VIRTUAL */

    /* Memory fabric imported ref — NV00FB_ALLOCATION_PARAMETERS (32 bytes) */
    case 0x00fb: return 32;

    /* Memory multicast fabric — uses its own params, not NV_MEMORY_ALLOCATION_PARAMS */
    case 0x00fc: return 64;  /* NV00FD_ALLOCATION_PARAMETERS — need to verify */

    /* VASPACE */
    case FERMI_VASPACE_A: return 56;  /* NV_VASPACE_ALLOCATION_PARAMETERS */

    /* Channel group */
    case KEPLER_CHANNEL_GROUP_A: return 20;  /* NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS */

    /* Channels — NV_CHANNEL_ALLOC_PARAMS */
    case 0xc46f: return 160; /* TURING_CHANNEL_GPFIFO_A */
    case 0xc56f: return 160; /* AMPERE_CHANNEL_GPFIFO_A */
    case 0xc86f: return 160; /* HOPPER_CHANNEL_GPFIFO_A */

    /* Graphics/compute objects — NV_GR_ALLOCATION_PARAMETERS (8 bytes) */
    case 0xc597: return  8;  /* TURING_A */
    case 0xc697: return  8;  /* AMPERE_A */
    case 0xc797: return  8;  /* ADA_A */
    case 0xcb97: return  8;  /* HOPPER_A */
    case 0xc5c0: return  8;  /* TURING_COMPUTE_A */
    case 0xc6c0: return  8;  /* AMPERE_COMPUTE_A */
    case 0xc7c0: return  8;  /* AMPERE_COMPUTE_B */
    case 0xc9c0: return  8;  /* ADA_COMPUTE_A */
    case 0xcbc0: return  8;  /* HOPPER_COMPUTE_A */

    /* 2D, inline-to-memory — NV_GR_ALLOCATION_PARAMETERS (8 bytes) */
    case 0x902d: return  8;  /* FERMI_TWOD_A */
    case 0xa140: return  8;  /* KEPLER_INLINE_TO_MEMORY_B */

    /* DMA copy — NVB0B5_ALLOCATION_PARAMETERS (4 bytes) */
    case 0xc5b5: return  4;  /* TURING_DMA_COPY_A */
    case 0xc6b5: return  4;  /* AMPERE_DMA_COPY_A */
    case 0xc7b5: return  4;  /* AMPERE_DMA_COPY_B */
    case 0xc8b5: return  4;  /* HOPPER_DMA_COPY_A */
    case 0xcbb5: return  4;  /* (if needed) */

    /* Video decode — NV_BSP_ALLOCATION_PARAMETERS (8 bytes) */
    case 0xb8b0: return  8;
    case 0xc4b0: return  8;
    case 0xc6b0: return  8;
    case 0xc7b0: return  8;
    case 0xc9b0: return  8;

    /* Video encode — NV_MSENC_ALLOCATION_PARAMETERS (8 bytes) */
    case 0xc4b7: return  8;
    case 0xc7b7: return  8;
    case 0xc9b7: return  8;

    /* P2P */
    case 0x503b: return 16;  /* NV503B_ALLOC_PARAMETERS */
    case 0x503c: return  8;  /* NV503C_ALLOC_PARAMETERS */

    /* Context share */
    case 0x9067: return 16;  /* NV_CTXSHARE_ALLOCATION_PARAMETERS */

    /* Display */
    case 0x9072: return  8;  /* NV9072_ALLOCATION_PARAMETERS */

    /* Event */
    case 0x0005: return 16;  /* NV0005_ALLOC_PARAMETERS */

    /* Semaphore surface */
    case 0x00da: return 16;  /* NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS */

    /* Memory fabric */
    case 0x00f8: return 32;  /* NV00F8_ALLOCATION_PARAMETERS */
    case 0x00fd: return 32;  /* NV00FD_ALLOCATION_PARAMETERS */

    /* Hopper usermode */
    case 0xc661: return  8;  /* NV_HOPPER_USERMODE_A_PARAMS */

    /* Confidential compute */
    case 0xcb33: return 16;  /* NV_CONFIDENTIAL_COMPUTE_ALLOC_PARAMS */

    /* Memory mapper */
    case 0x00fe: return  8;  /* NV_MEMORY_MAPPER_ALLOCATION_PARAMS */

    /* SM debugger */
    case 0x83de: return 16;  /* GT200_DEBUGGER alloc params */

    /* RM user shared data */
    case 0x00de: return  4;  /* NV00DE_ALLOC_PARAMETERS */

    /* No-params classes */
    case 0x90cc: return  0;  /* GF100_PROFILER */
    case 0xc461: return  0;  /* TURING_USERMODE_A */
    case 0xc361: return  0;  /* VOLTA_USERMODE_A */
    case 0xcba2: return  0;  /* HOPPER_SEC2_WORK_LAUNCH_A */
    case 0x0073: return  0;  /* NV04_DISPLAY_COMMON */
    case 0x208f: return  0;  /* NV20_SUBDEVICE_DIAG */
    case 0x9096: return  0;  /* GF100_ZBC_CLEAR */
    case 0x90e6: return  0;  /* GF100_SUBDEVICE_MASTER */

    default:     return 512; /* Conservative fallback */
    }
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

// Forward declare
static int guest_fd_to_handle(int guest_fd, u64 *out_handle);

/* -------------------------------------------------------------------------
 * V1→V2 ioctl rewriting for embedded userspace pointers
 *
 * Many RM_CONTROL commands come in V1 (with NvP64 userspace pointer) and
 * V2 (with inline array) variants. We intercept V1 in the guest driver
 * and rewrite to V2 before sending to the backend. On response, we copy
 * the inline result back to the guest's original userspace pointer.
 *
 * Two patterns:
 *
 * GET_CAPS V1: {NvU32 capsTblSize, pad(4), NvP64 capsTbl}  (16 bytes)
 *   - userptr at offset 8, no prefix to copy into V2
 *
 * GET_INFO V1: {NvU32 listSize, pad(4), NvP64 list}  (16 bytes)
 *   - userptr at offset 8, copy listSize (4 bytes) into V2 at offset 0
 *
 * CE GET_CAPS V1: {NvU32 ceEngineType, NvU32 capsTblSize, NvP64 capsTbl} (24 bytes)
 *   - userptr at offset 16, copy ceEngineType (4 bytes) into V2 at offset 0
 * ---------------------------------------------------------------------- */

struct v1v2_rewrite_entry {
    u32 v1_cmd;
    u32 v2_cmd;
    u32 v2_size;            /* sizeof V2 params struct */
    u32 v1_userptr_offset;  /* offset of NvP64 in V1 nested params */
    u32 v1_copy_prefix;     /* bytes to copy from V1 start into V2 start */
    u32 v2_data_offset;     /* offset where result data starts in V2 */
    u32 v2_data_size;       /* max result data bytes to copy back to guest ptr */
};

static const struct v1v2_rewrite_entry v1v2_table[] = {
    /* ---- GET_CAPS: V1 = {u32 capsTblSize, NvP64 capsTbl} ---- */
    /*                v1_cmd      v2_cmd      v2sz  ptr_off prefix d_off d_sz */

    /* Device-level GET_CAPS */
    /* FB_GET_CAPS (device) */
    { 0x00801301, 0x00801307,     3,     8,     0,    0,    3 },
    /* HOST_GET_CAPS (device) */
    { 0x00801401, 0x00801402,     3,     8,     0,    0,    3 },
    /* FIFO_GET_CAPS (device) */
    { 0x00801701, 0x00801713,     2,     8,     0,    0,    2 },
    /* FIFO_GET_CAPS (subdevice) */
    { 0x20801701, 0x20801713,     2,     8,     0,    0,    2 },
    /* GR_GET_CAPS */
    { 0x00801102, 0x00801109,    48,     8,     0,    0,   23 },
    /* MSENC_GET_CAPS (device) */
    { 0x00801b01, 0x00801b02,    12,     8,     0,    0,    6 },
    /* NVJPG_GET_CAPS (device) */
    { 0x00801f01, 0x00801f02,    16,     8,     0,    0,    9 },
    /* BSP_GET_CAPS (device) */
    { 0x00801c01, 0x00801c02,     8,     8,     0,    0,    8 },

    /* Subdevice-level GET_CAPS — critical for vkCreateDevice */
    /* GR_GET_CAPS (subdevice): V2 = {NvU8[23], pad(1), GR_ROUTE_INFO(16), NvBool(4), pad(4)} = 48 */
    { 0x20801202, 0x20801227,    48,     8,     0,    0,   23 },
    /* FIFO_GET_CAPS (subdevice): V2 = {NvU8[2], pad(2)} = 4, round up to 2 u32s */
    { 0x20801701, 0x20801713,     4,     8,     0,    0,    2 },
    /* FB_GET_CAPS (subdevice): V2 = {NvU8[3], pad(1)} = 4, round up to 1 u32 */
    { 0x20801301, 0x20801307,     4,     8,     0,    0,    3 },

    /* CE_GET_CAPS: V1 = {u32 ceEngineType, u32 capsTblSize, NvP64 capsTbl} */
    { 0x20802a01, 0x20802a03,     8,    16,     4,    4,    2 },

    /* ---- GET_INFO: V1 = {u32 listSize, NvP64 list} ---- */

    /* GR_GET_INFO (device): V2 = {u32 listSize, GR_INFO[59], GR_ROUTE_INFO} */
    { 0x00801104, 0x00801110,   496,     8,     4,    4,  472 },
    /* GR_GET_INFO (subdevice): V2 = {u32 listSize, GR_INFO[59], pad(4), GR_ROUTE_INFO(16)} */
    { 0x20801201, 0x20801228,   496,     8,     4,    4,  472 },
    /* FB_GET_INFO (subdevice): V2 = {u32 listSize, FB_INFO[128]} */
    { 0x20801302, 0x20801303,  1028,     8,     4,    4, 1024 },
    /* GPU_GET_INFO: V2 = {u32 listSize, GPU_INFO[70]} */
    { 0x20800101, 0x20800102,   564,     8,     4,    4,  560 },
    /* BUS_GET_INFO: V2 = {u32 listSize, BUS_INFO[52]} */
    { 0x20801802, 0x20801823,   420,     8,     4,    4,  416 },
    /* BIOS_GET_INFO: V2 = {u32 listSize, BIOS_INFO[15]} */
    { 0x20800802, 0x20800810,   124,     8,     4,    4,  120 },
};

static const struct v1v2_rewrite_entry *find_v1v2_rewrite(u32 cmd)
{
    int i;
    for (i = 0; i < ARRAY_SIZE(v1v2_table); i++) {
        if (v1v2_table[i].v1_cmd == cmd)
            return &v1v2_table[i];
    }
    return NULL;
}

/*
 * nv_ioctl_nested — handle ioctls with embedded userspace pointers.
 *
 * Copies both the outer struct and the nested param buffer from guest
 * userspace, sends them concatenated (outer + nested) to the backend.
 * On response, copies both back to guest userspace.
 *
 * For V1 GET_CAPS/GET_INFO commands, rewrites to V2 equivalents to avoid
 * double-nested userspace pointers.
 *
 * For certain RM_CONTROL commands (EXPORT_OBJECT_TO_FD, IMPORT_OBJECT_FROM_FD),
 * the nested params contain a raw fd that must be translated to a guest_handle
 * before sending to the backend (which then translates to a host fd).
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
  u32 copy_size;   /* how many bytes we actually copy from userspace */
  void *nested_buf = NULL;
  void *combined = NULL;
  u32 total_size;

  /* For fd-carrying RM_CONTROLs */
  int saved_nested_fd = -1;
  unsigned int nested_fd_offset = 0;

  /* For V1→V2 rewriting */
  const struct v1v2_rewrite_entry *rewrite = NULL;
  u64 saved_user_ptr = 0;    /* original guest userspace data pointer */
  u32 saved_user_data_size = 0; /* how many bytes guest expects back */
  u32 saved_v1_cmd = 0;
  u32 saved_v1_params_size = 0;

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

  copy_size = nested_size;

  if (user_ptr && copy_size == 0) {
    if (outer_size == RMALLOC_OUTER_SIZE) {
      u32 hClass;
      memcpy(&hClass, &outer[RMALLOC_HCLASS_OFFSET], sizeof(u32));
      copy_size = rmalloc_class_param_size(hClass);
      pr_debug("nv_ioctl_nested: hClass=0x%x paramsSize=0, copy_size=%u\n",
               hClass, copy_size);
    } else {
      copy_size = 512;
      pr_warn_once("nv_ioctl_nested: RM_CONTROL paramsSize==0 with non-null ptr, using fallback %u\n",
                   copy_size);
    }
  }

  /* Copy nested params from guest userspace if present */
  if (user_ptr && copy_size > 0) {
    if (copy_size > NV_MAX_PARAM_SIZE - outer_size)
      return -EINVAL;

    nested_buf = kmalloc(copy_size, GFP_KERNEL);
    if (!nested_buf)
      return -ENOMEM;

    if (copy_from_user(nested_buf, (void __user *)user_ptr, copy_size)) {
      ret = -EFAULT;
      goto out;
    }
  }

  /*
   * V1→V2 rewriting.
   *
   * Only applies to RM_CONTROL (outer_size == RMCTL_OUTER_SIZE).
   * Check if the cmd matches a V1 entry; if so, rewrite to V2.
   */
  if (outer_size == RMCTL_OUTER_SIZE && nested_buf) {
    u32 ctl_cmd;
    memcpy(&ctl_cmd, &outer[8], sizeof(u32));

    rewrite = find_v1v2_rewrite(ctl_cmd);
    if (rewrite) {
      void *v2_buf;
      u32 min_v1_size;

      /* Validate V1 nested is big enough to contain the userspace pointer */
      min_v1_size = rewrite->v1_userptr_offset + 8; /* NvP64 = 8 bytes */
      if (copy_size < min_v1_size) {
        pr_err("v1v2: V1 nested too small (%u < %u) for cmd 0x%x\n",
               copy_size, min_v1_size, ctl_cmd);
        rewrite = NULL;
        goto skip_rewrite;
      }

      /* Extract the userspace data pointer from V1 nested params */
      memcpy(&saved_user_ptr, nested_buf + rewrite->v1_userptr_offset,
             sizeof(u64));

      if (!saved_user_ptr) {
        pr_warn("v1v2: data pointer is NULL for cmd 0x%x, skipping rewrite\n",
                ctl_cmd);
        rewrite = NULL;
        goto skip_rewrite;
      }

      /*
       * For GET_INFO: the listSize at V1 offset 0 tells us how many items
       * the guest expects. Compute actual data bytes to copy back.
       * For GET_CAPS: capsTblSize at V1 offset 0 is the byte count.
       */
      if (rewrite->v1_copy_prefix > 0) {
        /* GET_INFO pattern: listSize is item count, each item is 8 bytes */
        u32 list_size;
        memcpy(&list_size, nested_buf, sizeof(u32));
        saved_user_data_size = min_t(u32, list_size * 8, rewrite->v2_data_size);
      } else {
        /* GET_CAPS pattern: capsTblSize is byte count */
        u32 caps_tbl_size;
        memcpy(&caps_tbl_size, nested_buf, sizeof(u32));
        saved_user_data_size = min_t(u32, caps_tbl_size, rewrite->v2_data_size);
      }

      saved_v1_cmd = ctl_cmd;
      memcpy(&saved_v1_params_size, &outer[size_offset], sizeof(u32));

      pr_debug("v1v2: rewriting cmd 0x%x → 0x%x (V2 size %u, data_back %u)\n",
               ctl_cmd, rewrite->v2_cmd,
               rewrite->v2_size, saved_user_data_size);

      /* Allocate V2 buffer, zeroed */
      v2_buf = kzalloc(rewrite->v2_size, GFP_KERNEL);
      if (!v2_buf) {
        ret = -ENOMEM;
        goto out;
      }

      /* Copy prefix from V1 into V2 (e.g., listSize or ceEngineType) */
      if (rewrite->v1_copy_prefix > 0 &&
          copy_size >= rewrite->v1_copy_prefix) {
        memcpy(v2_buf, nested_buf, rewrite->v1_copy_prefix);
      }

      /* Replace nested buffer with V2 */
      kfree(nested_buf);
      nested_buf = v2_buf;
      copy_size = rewrite->v2_size;

      /* Update outer: cmd → V2 cmd, paramsSize → V2 size */
      memcpy(&outer[8], &rewrite->v2_cmd, sizeof(u32));
      memcpy(&outer[size_offset], &rewrite->v2_size, sizeof(u32));
    }
  }
skip_rewrite:

  /*
   * Translate embedded fds in certain RM_CONTROL nested params.
   * Only when NOT doing a V1→V2 rewrite (mutually exclusive).
   */
  if (outer_size == RMCTL_OUTER_SIZE && nested_buf && !rewrite) {
    u32 ctl_cmd;
    memcpy(&ctl_cmd, &outer[8], sizeof(u32));

    if (ctl_cmd == 0x3d05 && copy_size >= 20) {
      int guest_fd;
      u64 handle;

      memcpy(&guest_fd, nested_buf + 16, sizeof(int));
      saved_nested_fd = guest_fd;
      nested_fd_offset = 16;

      ret = guest_fd_to_handle(guest_fd, &handle);
      if (ret) {
        pr_err("EXPORT_TO_FD: cannot translate guest fd %d: %ld\n",
               guest_fd, ret);
        goto out;
      }
      {
        u32 h32 = (u32)handle;
        memcpy(nested_buf + 16, &h32, sizeof(u32));
      }
      pr_debug("EXPORT_TO_FD: guest fd %d → handle %llu\n",
               guest_fd, handle);

    } else if (ctl_cmd == 0x3d06 && copy_size >= 4) {
      int guest_fd;
      u64 handle;

      memcpy(&guest_fd, nested_buf, sizeof(int));
      saved_nested_fd = guest_fd;
      nested_fd_offset = 0;

      ret = guest_fd_to_handle(guest_fd, &handle);
      if (ret) {
        pr_err("IMPORT_FROM_FD: cannot translate guest fd %d: %ld\n",
               guest_fd, ret);
        goto out;
      }
      {
        u32 h32 = (u32)handle;
        memcpy(nested_buf, &h32, sizeof(u32));
      }
      pr_debug("IMPORT_FROM_FD: guest fd %d → handle %llu\n",
               guest_fd, handle);
    }
  }

  /* Zero the pointer in outer — backend sets its own */
  memset(&outer[ptr_offset], 0, sizeof(u64));

  /* Build combined buffer: outer + nested */
  total_size = outer_size + (nested_buf ? copy_size : 0);
  combined = kmalloc(total_size, GFP_KERNEL);
  if (!combined) {
    ret = -ENOMEM;
    goto out;
  }

  memcpy(combined, outer, outer_size);
  if (nested_buf)
    memcpy(combined + outer_size, nested_buf, copy_size);

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

    if (rewrite) {
      /*
       * V1→V2 rewrite response path.
       */
      if (resp_total < outer_size) {
        pr_err("v1v2: response too short (%u < %u)\n",
               resp_total, outer_size);
        ret = -EIO;
        goto out;
      }

      /* Copy V2 result data back to guest's original userspace pointer */
      if (resp_total > outer_size && saved_user_ptr && saved_user_data_size > 0) {
        u32 resp_nested_size = resp_total - outer_size;
        u32 avail = 0;

        if (resp_nested_size > rewrite->v2_data_offset)
          avail = resp_nested_size - rewrite->v2_data_offset;

        if (avail > 0) {
          u32 copy_back = min_t(u32, saved_user_data_size, avail);
          if (copy_to_user((void __user *)saved_user_ptr,
                           resp_params + outer_size + rewrite->v2_data_offset,
                           copy_back)) {
            ret = -EFAULT;
            goto out;
          }
        }
      }

      /* Restore V1 outer: cmd, paramsSize, userspace pointer */
      memcpy(resp_params + 8, &saved_v1_cmd, sizeof(u32));
      memcpy(resp_params + size_offset, &saved_v1_params_size, sizeof(u32));
      memcpy(resp_params + ptr_offset, &user_ptr, sizeof(u64));

      if (copy_to_user(uarg, resp_params, outer_size)) {
        ret = -EFAULT;
        goto out;
      }

    } else if (resp_total >= outer_size) {
      /* Normal (non-rewrite) path */
      memcpy(resp_params + ptr_offset, &user_ptr, sizeof(u64));

      if (copy_to_user(uarg, resp_params, outer_size)) {
        ret = -EFAULT;
        goto out;
      }

      if (user_ptr && copy_size > 0) {
        u32 resp_nested = resp_total - outer_size;
        u32 copy_len = min_t(u32, resp_nested, copy_size);

        if (saved_nested_fd >= 0 && copy_len >= nested_fd_offset + 4) {
          memcpy(resp_params + outer_size + nested_fd_offset,
                 &saved_nested_fd, sizeof(int));
        }

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
 * nv_ioctl_fd_carrying
 * ---------------------------------------------------------------------- */

static long nv_ioctl_fd_carrying(struct file *filp, unsigned int cmd,
                                 unsigned long arg, unsigned int fd_offset)
{
  struct nv_file_ctx *ctx = filp->private_data;
  struct nv_dev *ndev = ctx->dev;
  unsigned int param_size = _IOC_SIZE(cmd);
  unsigned int escape = _IOC_NR(cmd);
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

  memcpy(&orig_fd, param_buf + fd_offset, sizeof(int));

  if (orig_fd == -1) {
    handle = ctx->guest_handle;
  } else {
    ret = guest_fd_to_handle(orig_fd, &handle);
    if (ret) {
      pr_err("nv_ioctl_fd_carrying: bad embedded fd %d: %ld\n", orig_fd, ret);
      goto out;
    }
  }

  {
    u32 handle32 = (u32)handle;
    memcpy(param_buf + fd_offset, &handle32, sizeof(u32));
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

  {
    u32 copy_len = min_t(u32, param_size, le32_to_cpu(iresp->param_size));
    void *resp_params = (char *)resp.resp_payload + sizeof(struct ioctl_resp);

    /* Restore original fd before copying back */
    if (copy_len >= fd_offset + 4)
      memcpy(resp_params + fd_offset, &orig_fd, sizeof(int));

    if (escape == 0x4E && le64_to_cpu(iresp->shm_length) > 0) {
      u64 shm_off = le64_to_cpu(iresp->shm_offset);
      u64 shm_len = le64_to_cpu(iresp->shm_length);
      u8 pgprot_val = iresp->pgprot;

      struct nv_mapping_info *mi = kmalloc(sizeof(*mi), GFP_KERNEL);
      if (mi) {
        mi->shm_offset = shm_off;
        mi->shm_length = shm_len;
        mi->pgprot = pgprot_val;
        spin_lock(&ndev->mappings_lock);
        list_add_tail(&mi->list, &ndev->mappings);
        spin_unlock(&ndev->mappings_lock);
        pr_info("nv_ioctl: pushed SHM mapping: offset=0x%llx len=0x%llx\n",
                shm_off, shm_len);
      }
    }

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
  unsigned int ioc_type = _IOC_TYPE(cmd);
  unsigned int param_size = _IOC_SIZE(cmd);

  pr_debug("nv_ioctl: escape=0x%x size=%u\n", escape, param_size);

  /* nvidia-modeset ioctls use type 'm' (0x6d), not 'F' (0x46).
   * They have embedded userspace pointers in the same way as RM_CONTROL:
   *   struct nvkms_ioctl_params {
   *     NvU32 cmd;       // offset 0
   *     NvU32 dataSize;  // offset 4
   *     NvU64 pData;     // offset 8 — userspace pointer
   *   };
   * Total: 16 bytes.
   */
  if (ioc_type == 0x6d) {
    return nv_ioctl_nested(filp, cmd, arg,
                           16,   /* outer_size */
                           8,    /* ptr_offset (pData) */
                           4);   /* size_offset (dataSize) */
  }

  /* Standard NV_ESC ioctls (type 'F' = 0x46) */
  switch (escape) {
    case NV_ESC_RM_CONTROL: /* NV_ESC_RM_CONTROL */
      return nv_ioctl_nested(filp, cmd, arg,
                             RMCTL_OUTER_SIZE, RMCTL_PTR_OFFSET, RMCTL_SIZE_OFFSET);
    case NV_ESC_RM_ALLOC: /* NV_ESC_RM_ALLOC */
      return nv_ioctl_nested(filp, cmd, arg,
                             RMALLOC_OUTER_SIZE, RMALLOC_PTR_OFFSET, RMALLOC_SIZE_OFFSET);
    case NV_ESC_RM_MAP_MEMORY: /* NV_ESC_RM_MAP_MEMORY — fd at offset 48 */
      return nv_ioctl_fd_carrying(filp, cmd, arg, 48);
    case NV_ESC_RM_ALLOC_MEMORY: /* RM_ALLOC_MEMORY — fd at offset 48 */
      return nv_ioctl_fd_carrying(filp, cmd, arg, 48);
    case NV_ESC_REGISTER_FD: /* NV_ESC_REGISTER_FD */
      return nv_ioctl_fd_carrying(filp, cmd, arg, 0);
    case NV_ESC_ALLOC_OS_EVENT: /* NV_ESC_ALLOC_OS_EVENT */
      return nv_ioctl_fd_carrying(filp, cmd, arg, 8);
    case NV_ESC_FREE_OS_EVENT: /* NV_ESC_FREE_OS_EVENT */
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

      /* Debug: log RM_MAP_MEMORY status */
      if (escape == 0x27 && copy_len >= 48) {
        u32 map_status;
        memcpy(&map_status, resp_params + 40, sizeof(u32)); /* NVOS33 status offset */
        pr_info("nv_ioctl: RM_MAP_MEMORY status=0x%x\n", map_status);
      }

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
