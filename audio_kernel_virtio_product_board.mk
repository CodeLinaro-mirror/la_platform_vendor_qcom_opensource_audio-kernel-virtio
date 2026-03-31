# Build audio kernel driver

ifneq (,$(filter gen4_gvm_gy gen4_gvm_gy_sgt gen5_gvm gen5_gvm_sgt gen5_gvm_gy auto_gen_prime, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)$(TARGET_BOARD_DERIVATIVE_SUFFIX)))
PRODUCT_PACKAGES  += $(KERNEL_MODULES_OUT)/msm_virtio_snd.ko
endif
