$(info Building Iago image with SYSLINUX support)

inc := $(call intermediates-dir-for,PACKAGING,iago_syslinux_deps)

$(inc)/syslinux: $(TARGET_SYSLINUX_FILES)
	$(hide) rm -rf $@
	$(hide) mkdir -p $@
	$(hide) $(ACP) -f $(TARGET_SYSLINUX_FILES) $@

IAGO_IMAGES_DEPS += \
	$(call intermediates-dir-for,EXECUTABLES,android_syslinux)/android_syslinux \
	$(inc)/syslinux \
	$(SYSLINUX_BASE)/gptmbr.bin \
	$(TARGET_SYSLINUX_CONFIG_TEMPLATE) \


