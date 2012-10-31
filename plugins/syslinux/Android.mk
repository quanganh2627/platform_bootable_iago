LOCAL_PATH := $(call my-dir)

# library for syslinux installation
include $(CLEAR_VARS)

LOCAL_MODULE := libiago_syslinux
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := iago-syslinux.c
LOCAL_CFLAGS := -Wall -Werror
LOCAL_C_INCLUDES := bootable/iago/include  external/iniparser/src

include $(BUILD_STATIC_LIBRARY)

# test program
#include $(CLEAR_VARS)

#LOCAL_MODULE := test_iago_syslinux
#LOCAL_MODULE_TAGS := optional
#LOCAL_SRC_FILES := test.c
#LOCAL_CFLAGS := -Wall -Werror
#LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../include/
#LOCAL_STATIC_LIBRARIES := libiago_syslinux libcutils liblog libc

#include $(BUILD_EXECUTABLE)
