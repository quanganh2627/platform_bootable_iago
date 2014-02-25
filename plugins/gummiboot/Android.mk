LOCAL_PATH := $(call my-dir)

# library for gummiboot installation
include $(CLEAR_VARS)

LOCAL_MODULE := libiago_gummiboot
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := gummiboot.c
LOCAL_CFLAGS := -Wall -Werror -DUEFI_ARCH=\"$(TARGET_UEFI_ARCH)\"
ifneq ($(TARGET_USE_MOKMANAGER),false)
LOCAL_CFLAGS += -DUSE_MOKMANAGER
endif
LOCAL_C_INCLUDES := bootable/iago/include  external/iniparser/src

include $(BUILD_STATIC_LIBRARY)

