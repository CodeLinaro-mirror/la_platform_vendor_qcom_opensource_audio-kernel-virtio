# SPDX-License-Identifier: GPL-2.0-only

LINUXINCLUDE    += -I${VIDEO_ROOT}/include/uapi \
                   -I${KERNEL_ROOT}/include

USERINCLUDE     += -I${VIDEO_ROOT}/include/uapi

ccflags-y := -I"$(src)/include/uapi"

msm_virtio_snd-objs := \
        virtio_card.o \
        virtio_chmap.o \
        virtio_ctl_msg.o \
        virtio_dc.o \
        virtio_event.o \
        virtio_jack.o \
        virtio_opsy_ctl_msg.o \
        virtio_pcm.o \
        virtio_pcm_msg.o \
        virtio_pcm_ops.o

obj-m += msm_virtio_snd.o
