$(info Building Iago image with SYSLINUX support)


IAGO_IMAGES_DEPS += \
	bootable/iago/plugins/syslinux/syslinux.template.cfg \
	$(call intermediates-dir-for,EXECUTABLES,android_syslinux)/android_syslinux \
	$(SYSLINUX_BASE)/android.c32 \
	$(SYSLINUX_BASE)/vesamenu.c32 \
	$(SYSLINUX_BASE)/gptmbr.bin \

