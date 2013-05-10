$(info Building Iago image with GummiBoot support)

IAGO_IMAGES_DEPS += $(GUMMIBOOT_EFI) \
		$(UEFI_SHIM_EFI) \

ifneq ($(TARGET_USE_MOKMANAGER),false)
IAGO_IMAGES_DEPS += $(MOKMANAGER_EFI)
endif
