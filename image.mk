ifeq ($(TARGET_USE_IAGO),true)

LOCAL_PATH := $(call my-dir)

iago_base := $(PRODUCT_OUT)/iago
iago_ramdisk_root := $(iago_base)/ramdisk
iago_iso_root := $(iago_base)/root
iago_ramdisk := $(iago_base)/ramdisk.img
iago_images_root := $(iago_base)/images
iago_images_sfs := $(iago_base)/images.sfs
iago_iso_image := $(PRODUCT_OUT)/liveimg.iso
iago_ini := $(iago_base)/iago.ini
iago_default_ini := $(iago_base)/iago-default.ini

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


iago_isolinux_files := \
	$(SYSLINUX_BASE)/isolinux.bin \
	$(SYSLINUX_BASE)/vesamenu.c32 \
	$(LOCAL_PATH)/splash.png \
	$(iago_base)/isolinux.cfg \

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

$(iago_base)/isolinux.cfg: $(LOCAL_PATH)/isolinux.cfg
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
		| $(MINIGZIP) \

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

# TODO: Add 'genisoimage' to the build
$(iago_iso_image): \
		$(LOCAL_PATH)/image.mk \
		$(iago_ramdisk) \
		$(iago_images_sfs) \
		$(iago_isolinux_files) \
		$(INSTALLED_KERNEL_TARGET) \
		$(HOST_OUT_EXECUTABLES)/isohybrid \
		| $(ACP) \

	$(hide) rm -rf $(iago_iso_root)
	$(hide) mkdir -p $(iago_iso_root)
	$(hide) $(ACP) -f $(iago_ramdisk) $(iago_iso_root)/ramdisk.img
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

.PHONY: liveimg
liveimg: $(iago_iso_image)

endif # TARGET_USE_IAGO
