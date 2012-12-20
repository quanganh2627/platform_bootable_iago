ifeq ($(TARGET_USE_IAGO),true)

LOCAL_PATH := $(call my-dir)

iago_base := $(PRODUCT_OUT)/iago
iago_ramdisk_root := $(iago_base)/ramdisk
iago_rootfs := $(iago_base)/root
iago_ramdisk := $(iago_base)/ramdisk.img
iago_images_root := $(iago_base)/images
iago_images_sfs := $(iago_base)/images.sfs
iago_fs_img := $(iago_base)/root.vfat
iago_img := $(PRODUCT_OUT)/live.img
iago_ini := $(iago_base)/iago.ini
iago_default_ini := $(iago_base)/iago-default.ini
iago_efi_dir := $(iago_rootfs)/EFI/BOOT/

define create-sfs
	$(hide) PATH=/sbin:/usr/sbin:$(PATH) mksquashfs $(1) $(2) -no-recovery -noappend
endef

IAGO_IMAGES_DEPS := \
	$(INSTALLED_BOOTIMAGE_TARGET) \
	$(INSTALLED_RECOVERYIMAGE_TARGET) \
	$(PRODUCT_OUT)/parted/parted \
	$(iago_base)/iagod \

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

# Build the iago-default.ini, which is options for non-interactive mode
$(iago_default_ini): \
		bootable/iago/installer/iago-default.ini \
		$(foreach plugin,$(TARGET_IAGO_PLUGINS),$(wildcard $(plugin)/iago-default.ini)) \
		$(TARGET_IAGO_DEFAULT_INI) \

	$(hide) mkdir -p $(dir $@)
	$(hide) cat $^ > $@

IAGO_IMAGES_DEPS += $(iago_default_ini)

iago_syslinux_files := \
	$(SYSLINUX_BASE)/vesamenu.c32 \
	$(SYSLINUX_BASE)/libcom32.c32 \
	$(SYSLINUX_BASE)/libcom32gpl.c32 \
	$(SYSLINUX_BASE)/libutil_com.c32 \
	$(SYSLINUX_BASE)/ldlinux.c32 \
	$(SYSLINUX_BASE)/libmenu.c32 \
	$(LOCAL_PATH)/splash.png \
	$(iago_base)/syslinux.cfg \


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

$(iago_base)/syslinux.cfg: $(LOCAL_PATH)/syslinux.cfg
	$(hide) mkdir -p $(iago_base)
	$(hide) sed "s|CMDLINE|$(BOARD_KERNEL_CMDLINE)|" $^ > $@

$(iago_ramdisk): \
		$(LOCAL_PATH)/image.mk \
		$(LOCAL_PATH)/init.nogui.rc \
		$(LOCAL_PATH)/init.iago.rc \
		$(INSTALLED_RAMDISK_TARGET) \
		$(MKBOOTFS) \
		$(iago_base)/preinit \
		$(iago_ini) \
		| $(MINIGZIP) $(ACP) \

	$(hide) rm -rf $(iago_ramdisk_root)
	$(hide) mkdir -p $(iago_ramdisk_root)
	$(hide) $(ACP) -rf $(TARGET_ROOT_OUT)/* $(iago_ramdisk_root)
	$(hide) mv $(iago_ramdisk_root)/init $(iago_ramdisk_root)/init2
	$(hide) $(ACP) -p $(iago_base)/preinit $(iago_ramdisk_root)/init
	$(hide) mkdir -p $(iago_ramdisk_root)/installmedia
	$(hide) mkdir -p $(iago_ramdisk_root)/tmp
	$(hide) mkdir -p $(iago_ramdisk_root)/mnt
	$(hide) echo "import init.iago.rc" >> $(iago_ramdisk_root)/init.rc
	$(hide) $(ACP) $(LOCAL_PATH)/init.nogui.rc $(iago_ramdisk_root)/init.nogui.rc
	$(hide) sed -i -r 's/^[\t ]*(mount_all|mount yaffs|mount ext).*//g' $(iago_ramdisk_root)/init*.rc
	$(hide) $(ACP) $(LOCAL_PATH)/init.iago.rc $(iago_ramdisk_root)
	$(hide) $(MKBOOTFS) $(iago_ramdisk_root) | $(MINIGZIP) > $@

ifeq ($(TARGET_KERNEL_ARCH),i386)
syslinux_efi_name := bootia32.efi
else
syslinux_efi_name := bootx64.efi
endif

$(iago_fs_img): \
		$(LOCAL_PATH)/image.mk \
		$(iago_ramdisk) \
		$(iago_images_sfs) \
		$(iago_syslinux_files) \
		$(INSTALLED_KERNEL_TARGET) \
		$(LOCAL_PATH)/make_vfatfs \
		$(SYSLINUX_BASE)/syslinux.efi \
		| $(ACP) \

	$(hide) rm -rf $(iago_rootfs)
	$(hide) mkdir -p $(iago_rootfs)
	$(hide) $(ACP) -f $(iago_ramdisk) $(iago_rootfs)/ramdisk.img
	$(hide) $(ACP) -f $(INSTALLED_KERNEL_TARGET) $(iago_rootfs)/kernel
	$(hide) touch $(iago_rootfs)/iago-cookie
	$(hide) $(ACP) -f $(iago_images_sfs) $(iago_rootfs)
	$(hide) mkdir -p $(iago_rootfs)/images
	$(hide) mkdir -p $(iago_efi_dir)
	$(hide) $(ACP) $(SYSLINUX_BASE)/syslinux.efi $(iago_efi_dir)/$(syslinux_efi_name)
	$(hide) $(ACP) -f $(iago_syslinux_files) $(iago_efi_dir)
	$(hide) $(LOCAL_PATH)/make_vfatfs $(iago_rootfs) $@

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

endif # TARGET_USE_IAGO
