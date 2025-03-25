load(":audio_modules.bzl", "audio_modules")
load(":module_mgr.bzl", "define_target_modules")
load("//msm-kernel:target_variants.bzl", "get_all_lunch_target_base_target_variants")
load("//msm-kernel:target_variants.bzl", "get_all_la_variants", "get_all_le_variants", "get_all_lxc_variants")

def define_gen5_gvm(t,v, lt=None):
    print(t)
    define_target_modules(
        target = "autoghgvm",
        variant = v,
        registry = audio_modules,
        modules = [ "msm_virtio_snd" ],
        lunch_target = lt
    )

def define_audio_target():
    for (t, v) in get_all_la_variants():
        print(t)
        print(v)
        if t == "autoghgvm":
            define_gen5_gvm(t,v)
