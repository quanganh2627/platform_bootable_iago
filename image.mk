ifeq ($(TARGET_USE_IAGO),true)

LOCAL_PATH := $(call my-dir)

iago_base := $(PRODUCT_OUT)/iago
iago_live_ramdisk_root := $(iago_base)/ramdisk_live
iago_nogui_ramdisk_root := $(iago_base)/ramdisk_nogui
iago_rootfs := $(iago_base)/root
iago_provision := $(iago_base)/provision_files
iago_nogui_ramdisk := $(iago_base)/ramdisk_live.img
iago_live_ramdisk := $(iago_base)/ramdisk_nogui.img
iago_images_root := $(iago_base)/images
iago_images_sfs := $(iago_base)/images.sfs
iago_fs_img := $(iago_base)/root.vfat
iago_img := $(PRODUCT_OUT)/live.img
iago_ini := $(iago_base)/iago.ini
iago_default_ini := $(iago_base)/iago-default.ini
iago_provision_ini := $(iago_base)/iago-provision.ini
iago_efi_dir := $(iago_rootfs)/EFI/BOOT/
iago_live_bootimage := $(iago_base)/liveboot.img
iago_interactive_bootimage := $(iago_base)/intboot.img
iago_automated_bootimage := $(iago_base)/autoboot.img

define create-sfs
	$(hide) PATH=/sbin:/usr/sbin:$(PATH) mksquashfs $(1) $(2) -no-recovery -noappend
endef

IAGO_IMAGES_DEPS := \
	$(INSTALLED_BOOTIMAGE_TARGET) \
	$(INSTALLED_RECOVERYIMAGE_TARGET) \

ifeq ($(TARGET_USERIMAGES_SPARSE_EXT_DISABLED),false)
# need to convert sparse images back to normal ext4
iago_sparse_images_deps += \
	$(INSTALLED_SYSTEMIMAGE) \
        $(INSTALLED_USERDATAIMAGE_TARGET) \

IAGO_IMAGES_DEPS_HOST += \
	$(HOST_OUT_EXECUTABLES)/simg2img \

else
IAGO_IMAGES_DEPS += \
	$(INSTALLED_SYSTEMIMAGE) \
        $(INSTALLED_USERDATAIMAGE_TARGET) \

endif

# Pull in all the plug-in makefiles, which can alter IAGO_IMAGES_DEPS to add
# additional files to the set of installation images
include $(foreach dir,$(TARGET_IAGO_PLUGINS),$(dir)/image.mk)

# Build the iago.ini, which the concatenation of any plugin-specific ini,
# the base Iago ini, and any board-specific ini file.
$(iago_ini): \
		bootable/iago/installer/iago.ini \
		$(foreach plugin,$(TARGET_IAGO_PLUGINS),$(wildcard $(plugin)/iago.ini)) \
		$(TARGET_IAGO_INI) \

	$(hide) mkdir -p $(dir $@)
	$(hide) cat $^ > $@

IAGO_IMAGES_DEPS += $(iago_ini)

# Build the iago-default.ini, which is overlay options for non-interactive mode
$(iago_default_ini): \
		bootable/iago/installer/iago-default.ini \
		$(foreach plugin,$(TARGET_IAGO_PLUGINS),$(wildcard $(plugin)/iago-default.ini)) \
		$(TARGET_IAGO_DEFAULT_INI) \

	$(hide) mkdir -p $(dir $@)
	$(hide) cat $^ > $@

IAGO_IMAGES_DEPS += $(iago_default_ini)

# Build the iago-provision.ini, which is overlay options for provisioning images.
# FIXME: This is just to change the config to rely on the embedded OTA update
# to provision /system and non-recovery boot images; would be nice to find a way
# to loopback mount OTA update zips so that we can use OTA updates in the live images
# as well.
$(iago_provision_ini): \
		bootable/iago/installer/iago-provision.ini \
		$(foreach plugin,$(TARGET_IAGO_PLUGINS),$(wildcard $(plugin)/iago-provision.ini)) \
		$(TARGET_IAGO_PROVISION_INI) \

	$(hide) mkdir -p $(dir $@)
	$(hide) cat $^ > $@

iago_efi_bins := $(GUMMIBOOT_EFI) $(UEFI_SHIM_EFI)
ifneq ($(TARGET_USE_MOKMANAGER),false)
iago_efi_bins += $(MOKMANAGER_EFI)
endif
INSTALLED_RADIOIMAGE_TARGET += $(iago_efi_bins)

# These all need to go in the target-files-package, as those are used to construct
# provisioning images.
iago_radio_zip := $(iago_base)/iago_provision_files.zip
$(iago_radio_zip): \
		$(iago_ini) \
		$(iago_default_ini) \
		$(iago_provision_ini) \
		$(iago_base)/preinit \
		$(iago_base)/iagod \
		$(iago_base)/efibootmgr \
		bootable/iago/init.provision.rc \
		bootable/iago/live_img_layout.conf \
		| $(ACP)
	$(hide) rm -rf $(iago_provision)
	$(hide) mkdir -p $(iago_provision)
	$(hide) $(ACP) $^ $(iago_provision)
	$(hide) zip -j $@ $(iago_provision)/*

INSTALLED_RADIOIMAGE_TARGET += $(iago_radio_zip)

$(iago_images_sfs): \
		$(IAGO_IMAGES_DEPS) \
		$(IAGO_IMAGES_DEPS_HOST) \
		$(HOST_OUT_EXECUTABLES)/simg2img \
		$(iago_sparse_images_deps) \
		| $(ACP) \

	$(hide) rm -rf $(iago_images_root)
	$(hide) mkdir -p $(iago_images_root)
	$(hide) mkdir -p $(dir $@)
	$(hide) $(ACP) -rpf $(IAGO_IMAGES_DEPS) $(iago_images_root)
ifeq ($(TARGET_USERIMAGES_SPARSE_EXT_DISABLED),false)
	$(hide) $(foreach _simg,$(iago_sparse_images_deps), \
		$(HOST_OUT_EXECUTABLES)/simg2img $(_simg) $(iago_images_root)/`basename $(_simg)`; \
		)
endif
	$(call create-sfs,$(iago_images_root),$@)

# Special tools that we need that aren't staged in /system
iago_sbin_files := \
	$(iago_base)/iagod \
	$(iago_base)/ntfsresize \
	$(iago_base)/efibootmgr

$(iago_live_ramdisk): \
		$(LOCAL_PATH)/image.mk \
		$(LOCAL_PATH)/init.iago.rc \
		$(INSTALLED_RAMDISK_TARGET) \
		$(MKBOOTFS) \
		$(iago_base)/preinit \
		$(iago_sbin_files) \
		$(iago_ini) \
		| $(MINIGZIP) $(ACP) \

	$(hide) rm -rf $(iago_live_ramdisk_root)
	$(hide) mkdir -p $(iago_live_ramdisk_root)
	$(hide) $(ACP) -rf $(TARGET_ROOT_OUT)/* $(iago_live_ramdisk_root)
	$(hide) $(ACP) -f $(iago_sbin_files) $(iago_live_ramdisk_root)/sbin
	$(hide) $(ACP) -f $(iago_ini) $(iago_live_ramdisk_root)
	$(hide) mv $(iago_live_ramdisk_root)/init $(iago_live_ramdisk_root)/init2
	$(hide) $(ACP) -p $(iago_base)/preinit $(iago_live_ramdisk_root)/init
	$(hide) mkdir -p $(iago_live_ramdisk_root)/installmedia
	$(hide) mkdir -p $(iago_live_ramdisk_root)/tmp
	$(hide) mkdir -p $(iago_live_ramdisk_root)/mnt
	$(hide) echo "import init.iago.rc" >> $(iago_live_ramdisk_root)/init.rc
	$(hide) sed -i -r 's/^[\t ]*(mount_all|mount yaffs|mount ext).*//g' $(iago_live_ramdisk_root)/init*.rc
	$(hide) $(ACP) $(LOCAL_PATH)/init.iago.rc $(iago_live_ramdisk_root)
	$(hide) $(MKBOOTFS) $(iago_live_ramdisk_root) | $(MINIGZIP) > $@

$(iago_nogui_ramdisk): \
		$(LOCAL_PATH)/image.mk \
		$(LOCAL_PATH)/init.nogui.rc \
		$(LOCAL_PATH)/init.iago.rc \
		$(INSTALLED_RAMDISK_TARGET) \
		$(MKBOOTFS) \
		$(iago_base)/preinit \
		$(iago_sbin_files) \
		$(iago_ini) \
		$(iago_default_ini) \
		| $(MINIGZIP) $(ACP) \

	$(hide) rm -rf $(iago_nogui_ramdisk_root)
	$(hide) mkdir -p $(iago_nogui_ramdisk_root)
	$(hide) $(ACP) -rf $(TARGET_ROOT_OUT)/* $(iago_nogui_ramdisk_root)
	$(hide) $(ACP) -f $(iago_sbin_files) $(iago_nogui_ramdisk_root)/sbin
	$(hide) $(ACP) -f $(iago_ini) $(iago_default_ini) $(iago_nogui_ramdisk_root)
	$(hide) mv $(iago_nogui_ramdisk_root)/init $(iago_nogui_ramdisk_root)/init2
	$(hide) $(ACP) -p $(iago_base)/preinit $(iago_nogui_ramdisk_root)/init
	$(hide) mkdir -p $(iago_nogui_ramdisk_root)/installmedia
	$(hide) mkdir -p $(iago_nogui_ramdisk_root)/tmp
	$(hide) mkdir -p $(iago_nogui_ramdisk_root)/mnt
	$(hide) $(ACP) $(LOCAL_PATH)/init.nogui.rc $(iago_nogui_ramdisk_root)/init.rc
	$(hide) $(ACP) $(LOCAL_PATH)/init.iago.rc $(iago_nogui_ramdisk_root)
	$(hide) $(MKBOOTFS) $(iago_nogui_ramdisk_root) | $(MINIGZIP) > $@

$(iago_live_bootimage): \
		$(LOCAL_PATH)/image.mk \
		$(INSTALLED_KERNEL_TARGET) \
		$(iago_live_ramdisk) \
		$(MKBOOTIMG) \

	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
			--ramdisk $(iago_live_ramdisk) \
			--cmdline "$(BOARD_KERNEL_CMDLINE) androidboot.iago.ini=/iago.ini androidboot.iago.gui=1" \
			$(BOARD_MKBOOTIMG_ARGS) \
			--output $@

$(iago_interactive_bootimage): \
		$(LOCAL_PATH)/image.mk \
		$(INSTALLED_KERNEL_TARGET) \
		$(iago_nogui_ramdisk) \
		$(MKBOOTIMG) \

	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
			--ramdisk $(iago_nogui_ramdisk) \
			--cmdline "$(filter-out quiet vt.init_hide=%,$(BOARD_KERNEL_CMDLINE)) quiet vt.init_hide=0 androidboot.iago.ini=/iago.ini androidboot.iago.cli=1" \
			$(BOARD_MKBOOTIMG_ARGS) \
			--output $@

$(iago_automated_bootimage): \
		$(LOCAL_PATH)/image.mk \
		$(INSTALLED_KERNEL_TARGET) \
		$(iago_nogui_ramdisk) \
		$(MKBOOTIMG) \

	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
			--ramdisk $(iago_nogui_ramdisk) \
			--cmdline "$(filter-out vt.init_hide=%,$(BOARD_KERNEL_CMDLINE)) vt.init_hide=0 androidboot.iago.ini=/iago.ini,/iago-default.ini" \
			$(BOARD_MKBOOTIMG_ARGS) \
			--output $@

ifeq ($(TARGET_KERNEL_ARCH),i386)
efi_default_name := bootia32.efi
else
efi_default_name := bootx64.efi
endif

iago_loader_configs := \
	$(iago_base)/0live.conf \
        $(iago_base)/1install.conf \
	$(iago_base)/2interactive.conf \
	$(if $(LOCKDOWN_EFI),$(iago_base)/3secureboot.conf)

ifneq ($(TARGET_USE_MOKMANAGER),false)
iago_loader_configs += $(iago_base)/4mokmanager.conf
endif

$(iago_base)/%.conf: $(LOCAL_PATH)/loader/%.conf.in
	$(hide) mkdir -p $(iago_base)
	$(hide) sed "s|CMDLINE|$(BOARD_KERNEL_CMDLINE)|" $^ > $@

iago_efi_loader := $(iago_rootfs)/loader

$(iago_fs_img): \
		$(LOCAL_PATH)/image.mk \
		$(iago_live_bootimage) \
		$(iago_interactive_bootimage) \
		$(iago_automated_bootimage) \
		$(iago_images_sfs) \
		$(iago_loader_configs) \
		$(iago_efi_bins) \
		$(LOCAL_PATH)/tools/make_vfatfs \
		$(LOCAL_PATH)/loader/loader.conf \
		$(LOCKDOWN_EFI) \
		| $(ACP) \

	$(hide) rm -rf $(iago_rootfs)
	$(hide) mkdir -p $(iago_rootfs)
	$(hide) $(ACP) -f $(iago_live_bootimage) $(iago_rootfs)/liveboot.img
	$(hide) $(ACP) -f $(iago_interactive_bootimage) $(iago_rootfs)/intboot.img
	$(hide) $(ACP) -f $(iago_automated_bootimage) $(iago_rootfs)/autoboot.img
	$(hide) touch $(iago_rootfs)/iago-cookie
	$(hide) $(ACP) -f $(iago_images_sfs) $(iago_rootfs)
	$(hide) mkdir -p $(iago_rootfs)/images
	$(hide) mkdir -p $(iago_efi_dir)
	$(hide) mkdir -p $(iago_efi_loader)/entries
	$(hide) $(ACP) $(UEFI_SHIM_EFI) $(iago_efi_dir)/$(efi_default_name)
	$(hide) $(if $(LOCKDOWN_EFI),$(ACP) $(LOCKDOWN_EFI) $(iago_efi_dir))
	$(hide) $(ACP) $(iago_efi_bins) $(iago_efi_dir)
	$(hide) $(ACP) $(LOCAL_PATH)/loader/loader.conf $(iago_efi_loader)/loader.conf
	$(hide) $(ACP) -f $(iago_loader_configs) $(iago_efi_loader)/entries/
	$(hide) $(LOCAL_PATH)/tools/make_vfatfs $(iago_rootfs) $@

edit_mbr := $(HOST_OUT_EXECUTABLES)/editdisklbl

$(iago_img): \
		$(LOCAL_PATH)/image.mk \
		$(iago_fs_img) \
		$(edit_mbr) \
		$(LOCAL_PATH)/live_img_layout.conf \

	@echo "Creating IAGO Live image: $@"
	$(hide) rm -f $@
	$(hide) touch $@
	$(hide) $(edit_mbr) -v -i $@ \
		-l $(LOCAL_PATH)/live_img_layout.conf \
		liveimg=$(iago_fs_img)

.PHONY: liveimg
liveimg: $(iago_img)

# Build liveimg by default when 'make' is run
droidcore: $(iago_img)

# Put the live image in out/dist when 'make dist' is run
$(call dist-for-goals,droidcore,$(iago_img):$(TARGET_PRODUCT)-live-$(FILE_NAME_TAG).img)

# The following rules are for constructing an Iago image which boots in
# legacy mode. You CANNOT perform EFI installations even if you use an
# EFI bootloader plug-in, as the necessary call to efibootmgr can't be
# made without available EFI runtime services. You should use a legacy
# bootloader plugin.

iago_iso_root := $(iago_base)/legacyroot
iago_legacy_image := $(PRODUCT_OUT)/legacy.iso

iago_isolinux_files := \
	$(SYSLINUX_BASE)/isolinux.bin \
	$(SYSLINUX_BASE)/vesamenu.c32 \
	$(iago_base)/isolinux.cfg \

$(iago_base)/isolinux.cfg: $(LOCAL_PATH)/isolinux.cfg
	$(hide) mkdir -p $(iago_base)
	$(hide) sed "s|CMDLINE|$(BOARD_KERNEL_CMDLINE)|" $^ > $@

$(iago_legacy_image): \
		$(LOCAL_PATH)/image.mk \
		$(iago_nogui_ramdisk) \
		$(iago_live_ramdisk) \
		$(iago_images_sfs) \
		$(iago_isolinux_files) \
		$(INSTALLED_KERNEL_TARGET) \
		$(HOST_OUT_EXECUTABLES)/isohybrid \
		| $(ACP) \

	$(hide) rm -rf $(iago_iso_root)
	$(hide) mkdir -p $(iago_iso_root)
	$(hide) $(ACP) -f $(iago_live_ramdisk) $(iago_iso_root)/ramdisk_live.img
	$(hide) $(ACP) -f $(iago_nogui_ramdisk) $(iago_iso_root)/ramdisk_nogui.img
	$(hide) $(ACP) -f $(INSTALLED_KERNEL_TARGET) $(iago_iso_root)/kernel
	$(hide) touch $(iago_iso_root)/iago-cookie
	$(hide) $(ACP) -f $(iago_images_sfs) $(iago_iso_root)
	$(hide) mkdir -p $(iago_iso_root)/isolinux
	$(hide) mkdir -p $(iago_iso_root)/images
	$(hide) $(ACP) -f $(iago_isolinux_files) $(iago_iso_root)/isolinux/
	$(hide) genisoimage -vJURT -b isolinux/isolinux.bin -c isolinux/boot.cat \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-input-charset utf-8 -V "IAGO Android Live/Installer CD" \
		-o $@ $(iago_iso_root)
	$(hide) $(HOST_OUT_EXECUTABLES)/isohybrid $@

.PHONY: legacyimg
legacyimg: $(iago_legacy_image)

# Build and place legacy image in out/dist/ when 'make dist' is run.
$(call dist-for-goals,droidcore,$(iago_legacy_image):$(TARGET_PRODUCT)-legacyimg-$(FILE_NAME_TAG).iso)

endif # TARGET_USE_IAGO
