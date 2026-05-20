AUDIO_DLKM_ENABLE := false
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
  ifeq ($(TARGET_KERNEL_DLKM_AUDIO_OVERRIDE),true)
    AUDIO_DLKM_ENABLE := true
  endif
else
  AUDIO_DLKM_ENABLE := true
endif

ifeq ($(TARGET_DISABLE_AUDIO_VIRTIO), true)
  AUDIO_DLKM_ENABLE := false
endif

ifeq ($(AUDIO_DLKM_ENABLE), true)
  ifneq (,$(filter gen4_gvm_gy gen4_gvm_gy_sgt gen5_gvm gen5_gvm_cmu gen5_gvm_sgt gen5_gvm_gy auto_gen_prime, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)$(TARGET_BOARD_DERIVATIVE_SUFFIX)))
      include vendor/qcom/opensource/audio-kernel-virtio/audio_kernel_modules.mk
  endif
  BOARD_VENDOR_KERNEL_MODULES += $(AUDIO_KERNEL_MODULES)
endif
