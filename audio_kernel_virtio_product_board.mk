# Build audio kernel driver

ifeq ($(TARGET_USES_GY), true)
PRODUCT_PACKAGES  += $(KERNEL_MODULES_OUT)/msm_virtio_snd.ko
endif
