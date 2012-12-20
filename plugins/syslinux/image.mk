$(info Building Iago image with SYSLINUX support)

inc := $(call intermediates-dir-for,PACKAGING,iago_syslinux_deps)

TARGET_SYSLINUX_FILES += $(SYSLINUX_BASE)/syslinux.efi \
			 $(INSTALLED_KERNEL_TARGET) \
			 $(INSTALLED_RAMDISK_TARGET) \

$(inc)/syslinux: $(TARGET_SYSLINUX_FILES)
	$(hide) rm -rf $@
	$(hide) mkdir -p $@
	$(hide) $(ACP) -f $(TARGET_SYSLINUX_FILES) $@

IAGO_IMAGES_DEPS += \
	$(inc)/syslinux \
	$(TARGET_SYSLINUX_CONFIG_TEMPLATE) \


