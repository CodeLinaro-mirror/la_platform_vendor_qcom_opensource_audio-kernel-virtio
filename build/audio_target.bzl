load(":audio_modules.bzl", "audio_modules")
load(":module_mgr.bzl", "define_target_modules")

def define_gen5_gvm():
    define_target_modules(
        target = "autogvm",
        variants = ["consolidate", "perf", "debug-defconfig", "defconfig"],
        registry = audio_modules,
        modules = [
            "msm_virtio_snd",
        ]
    )

def define_audio_target():
    define_gen5_gvm()
