obj-$(CONFIG_VIRTIO_GPU_NV) += virtio_gpu_nv.o
virtio_gpu_nv-y := virtio_gpu_nv_main.o virtio_gpu_nv_ioctl.o virtio_gpu_nv_mmap.o virtio_gpu_nv_vq.o

# NVIDIA open-gpu-kernel-modules include paths
# From drivers/virtio/gpu_nv/ -> ../../.. = linux-6.12.76 root
ccflags-y += -I$(obj)/../../../../open-gpu-kernel-modules/src/nvidia/arch/nvalloc/unix/include
ccflags-y += -I$(obj)/../../../../open-gpu-kernel-modules/src/common/sdk/nvidia/inc
ccflags-y += -I$(obj)/../../../../open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class
