AUDIO_SELECT :=

LOCAL_PATH := $(call my-dir)

LOCAL_MODULE_DDK_BUILD := true

include $(CLEAR_VARS)

# This makefile is only for DLKM
ifneq ($(findstring vendor, $(LOCAL_PATH)),)

ifneq ($(findstring opensource, $(LOCAL_PATH)),)
    AUDIO_BLD_DIR := $(abspath .)/vendor/qcom/opensource/audio-kernel-virtio
endif

DLKM_DIR := $(TOP)/device/qcom/common/dlkm

# Build virtio_snd.ko as msm_virtio_snd.ko
###########################################################
# This is set once per LOCAL_PATH, not per (kernel) module
KBUILD_OPTIONS := AUDIO_ROOT=$(AUDIO_BLD_DIR)
KBUILD_OPTIONS += MODNAME=msm_virtio_snd
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
KBUILD_OPTION += $(AUDIO_SELECT)

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE := msm_virtio_snd.ko
LOCAL_MODULE_KBUILD_NAME := msm_virtio_snd.ko
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)


include $(DLKM_DIR)/Build_external_kernelmodule.mk
###########################################################
endif # DLKM check
