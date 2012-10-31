LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := droidboot.c
LOCAL_CFLAGS := -W -Wall -Werror
LOCAL_MODULE := libiago_droidboot
LOCAL_MODULE_TAGS := optional
LOCAL_C_INCLUDES := bootable/iago/include  external/iniparser/src

include $(BUILD_STATIC_LIBRARY)
