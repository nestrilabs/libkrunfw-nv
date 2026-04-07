obj-$(CONFIG_DRM_NVIDIA_STUB) += nvidia-drm-stub.o
nvidia-drm-stub-y := nvidia_drm_stub.o

# NVIDIA open-gpu-kernel-modules include paths
# From drivers/gpu/drm/nvidia-drm-stub/ -> ../../.. = linux-6.12.76 root
ccflags-y += -I$(obj)/../../../../open-gpu-kernel-modules/kernel-open
ccflags-y += -I$(obj)/../../../../open-gpu-kernel-modules/kernel-open/nvidia-drm
ccflags-y += -I$(obj)/../../../../open-gpu-kernel-modules/src/common/sdk/nvidia/inc
