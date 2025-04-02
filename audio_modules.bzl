load(":module_mgr.bzl", "create_module_registry")

audio_modules = create_module_registry([":audio_headers"])
# ------------------------------------ AUDIO MODULE DEFINITIONS ---------------------------------
# >>>> VIRTIO MODULES <<<<
audio_modules.register(
    name = "msm_virtio_snd",
    srcs = [
        "virtio_card.c",
        "virtio_chmap.c",
        "virtio_ctl_msg.c",
        "virtio_dc.c",
        "virtio_event.c",
        "virtio_jack.c",
        "virtio_opsy_ctl_msg.c",
        "virtio_pcm.c",
        "virtio_pcm_msg.c",
        "virtio_pcm_ops.c"
    ],
    config_option = "CONFIG_PM_SLEEP"
)
