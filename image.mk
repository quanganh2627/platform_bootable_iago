ifeq ($(TARGET_USE_IAGO),true)

LOCAL_PATH := $(call my-dir)

iago_base := $(PRODUCT_OUT)/iago
iago_ramdisk_root := $(iago_base)/ramdisk
iago_iso_root := $(iago_base)/root
iago_ramdisk := $(iago_base)/ramdisk.img
iago_iso_image := $(PRODUCT_OUT)/liveimg.iso

system_sfs := $(iago_base)/system.sfs
data_sfs := $(iago_base)/userdata.sfs

define create-sfs
	$(hide) PATH=/sbin:/usr/sbin:$(PATH) mksquashfs $(1) $(2) -no-recovery -noappend
endef

$(system_sfs): $(INSTALLED_SYSTEMIMAGE)
	$(hide) mkdir -p $(dir $@)
	$(call create-sfs,$(TARGET_OUT),$@)

$(data_sfs): $(INSTALLED_USERDATAIMAGE_TARGET)
	$(hide) mkdir -p $(dir $@)
	$(call create-sfs,$(TARGET_OUT_DATA),$@)

iago_images := \
	$(system_sfs) \
	$(data_sfs) \
	$(INSTALLED_BOOTIMAGE_TARGET) \
	$(INSTALLED_RECOVERYIMAGE_TARGET) \

ifeq ($(TARGET_STAGE_DROIDBOOT),true)
iago_images += $(DROIDBOOT_BOOTIMAGE)
endif

iago_isolinux_files := \
	$(SYSLINUX_BASE)/isolinux.bin \
	$(SYSLINUX_BASE)/vesamenu.c32 \
	$(LOCAL_PATH)/splash.png \
	$(iago_base)/isolinux.cfg \

$(iago_base)/isolinux.cfg: $(LOCAL_PATH)/isolinux.cfg
	$(hide) mkdir -p $(iago_base)
	$(hide) sed "s|CMDLINE|$(BOARD_KERNEL_CMDLINE)|" $^ > $@

$(iago_ramdisk): \
		$(LOCAL_PATH)/image.mk \
		$(INSTALLED_RAMDISK_TARGET) \
		$(MKBOOTFS) \
		$(iago_base)/preinit \
		| $(MINIGZIP) \

	$(hide) rm -rf $(iago_ramdisk_root)
	$(hide) mkdir -p $(iago_ramdisk_root)
	$(hide) $(ACP) -rf $(TARGET_ROOT_OUT)/* $(iago_ramdisk_root)
	$(hide) mv $(iago_ramdisk_root)/init $(iago_ramdisk_root)/init2
	$(hide) $(ACP) $(iago_base)/preinit $(iago_ramdisk_root)/init
	$(hide) $(ACP) $(LOCAL_PATH)/init.iago.rc $(iago_ramdisk_root)
	$(hide) mkdir -p $(iago_ramdisk_root)/installmedia
	$(hide) echo "import init.iago.rc" >> $(iago_ramdisk_root)/init.rc
	$(hide) $(MKBOOTFS) $(iago_ramdisk_root) | $(MINIGZIP) > $@

# TODO: Add 'genisoimage' to the build
$(iago_iso_image): \
		$(LOCAL_PATH)/image.mk \
		$(iago_ramdisk) \
		$(iago_images) \
		$(iago_isolinux_files) \
		$(INSTALLED_KERNEL_TARGET) \
		$(HOST_OUT_EXECUTABLES)/isohybrid \
		| $(ACP) \

	$(hide) rm -rf $(iago_iso_root)
	$(hide) mkdir -p $(iago_iso_root)
	$(hide) $(ACP) -f $(iago_ramdisk) $(iago_iso_root)/ramdisk.img
	$(hide) $(ACP) -f $(INSTALLED_KERNEL_TARGET) $(iago_iso_root)/kernel
	$(hide) touch $(iago_iso_root)/iago-cookie
	$(hide) mkdir -p $(iago_iso_root)/images
	$(hide) $(ACP) -f $(iago_images) $(iago_iso_root)/images
	$(hide) mkdir -p $(iago_iso_root)/isolinux
	$(hide) $(ACP) -f $(iago_isolinux_files) $(iago_iso_root)/isolinux/
	$(hide) genisoimage -vJURT -b isolinux/isolinux.bin -c isolinux/boot.cat \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-input-charset utf-8 -V "IAGO Android Live/Installer CD" \
		-o $@ $(iago_iso_root)
	$(hide) $(HOST_OUT_EXECUTABLES)/isohybrid $@

.PHONY: liveimg
liveimg: $(iago_iso_image)

endif # TARGET_USE_IAGO
