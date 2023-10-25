# Build audio kernel driver

ifeq ($(call is-board-platform-in-list,msmnile gen4), true)
ifneq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), msmnile_gvmq gen4_gvm))
PRODUCT_PACKAGES  += $(KERNEL_MODULES_OUT)/msm_virtio_snd.ko
endif
endif
