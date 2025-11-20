# Build audio kernel driver
ifeq ($(TARGET_USES_QMAA),true)
ifeq ($(TARGET_USES_QMAA_OVERRIDE_AUDIO),true)
BUILD_AUDIO_MODULES := true
else
BUILD_AUDIO_MODULES := false
endif
else
BUILD_AUDIO_MODULES := true
endif

ifeq ($(BUILD_AUDIO_MODULES),true)
ifeq ($(TARGET_BOARD_AUTO),true)

ifneq (,$(filter gen4_gvm_gy gen5_gvm gen5_gvm_sgt gen5_gvm_gy, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)$(TARGET_BOARD_DERIVATIVE_SUFFIX)))
AUDIO_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/msm_virtio_snd.ko
endif

endif
endif
