# Build audio kernel driver

ifeq ($(call is-board-platform-in-list,msmnile), true)
ifneq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), msmnile_gvmq))
PRODUCT_PACKAGES  += $(KERNEL_MODULES_OUT)/msm_virtio_snd.ko
endif
endif