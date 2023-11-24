AUDIO_DLKM_ENABLE := false
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
  ifeq ($(TARGET_KERNEL_DLKM_AUDIO_OVERRIDE),true)
    AUDIO_DLKM_ENABLE := true
  endif
else
  AUDIO_DLKM_ENABLE := true
endif

ifeq ($(TARGET_USES_AUDIOLITE), true)
  AUDIO_DLKM_ENABLE := false
endif

ifeq ($(AUDIO_DLKM_ENABLE), true)
  ifeq ($(TARGET_USES_GY), true)
      include vendor/qcom/opensource/audio-kernel-virtio/audio_kernel_modules.mk
  endif
  BOARD_VENDOR_KERNEL_MODULES += $(AUDIO_KERNEL_MODULES)
endif
