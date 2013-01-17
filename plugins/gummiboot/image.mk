$(info Building Iago image with GummiBoot support)

IAGO_IMAGES_DEPS += $(GUMMIBOOT_EFI) \
		$(INSTALLED_KERNEL_TARGET) \
		$(INSTALLED_RAMDISK_TARGET) \

