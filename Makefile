# SPDX-License-Identifier: GPL-2.0+

KBUILD_OPTIONS+= AUDIO_ROOT=$(KERNEL_SRC)/$(M)

AUDIO_ROOT=$(KERNEL_SRC)/$(M)
AUDIO_COMPILE_TIME = $(shell date)
AUDIO_COMPILE_BY = $(shell whoami | sed 's/\\/\\\\/')
AUDIO_COMPILE_HOST = $(shell uname -n)
AUDIO_GEN_PATH = $(AUDIO_ROOT)/audio_generated_h

all: modules

$(AUDIO_GEN_PATH): $(shell find . -type f \( -iname \*.c -o -iname \*.h -o -iname \*.mk \))
	echo '#define AUDIO_COMPILE_TIME "$(AUDIO_COMPILE_TIME)"' > $(AUDIO_GEN_PATH)
	echo '#define AUDIO_COMPILE_BY "$(AUDIO_COMPILE_BY)"' >> $(AUDIO_GEN_PATH)
	echo '#define AUDIO_COMPILE_HOST "$(AUDIO_COMPILE_HOST)"' >> $(AUDIO_GEN_PATH)

modules: $(AUDIO_GEN_PATH)
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	rm -rf *.0 *.ko *.mod.c *~ .*.cmd Modules.symvers
	rm -rf .tmp_versions
