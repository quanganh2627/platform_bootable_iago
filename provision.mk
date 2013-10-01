ifeq ($(TARGET_USE_IAGO),true)

LOCAL_PATH := $(call my-dir)

BUILT_PROVISIONING_IMAGE := $(PRODUCT_OUT)/provision.img
pftf := $(LOCAL_PATH)/tools/provision_from_target_files

$(BUILT_PROVISIONING_IMAGE): \
			$(LOCAL_PATH)/provision.mk \
			$(BUILT_TARGET_FILES_PACKAGE) \
			$(INTERNAL_OTA_PACKAGE_TARGET) \
			$(pftf) \
			$(DISTTOOLS) \
			$(LOCAL_PATH)/tools/make_vfatfs \

	$(hide) $(pftf) --verbose \
		        --ota_update $(INTERNAL_OTA_PACKAGE_TARGET) \
			$(BUILT_TARGET_FILES_PACKAGE) \
			$@

.PHONY: provimg
provimg: $(BUILT_PROVISIONING_IMAGE)

# Put provision.img in out/dist when 'make dist' is run
$(call dist-for-goals,droidcore,$(BUILT_PROVISIONING_IMAGE):$(TARGET_PRODUCT)-provision-$(FILE_NAME_TAG).img)

endif
