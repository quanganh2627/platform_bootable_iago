ifeq ($(TARGET_USE_IAGO),true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := preinit
LOCAL_MODULE_TAGS := eng
LOCAL_SRC_FILES := preinit.c
LOCAL_CFLAGS := -Wall -Werror
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_STATIC_LIBRARIES := libcutils libc
LOCAL_MODULE_PATH := $(PRODUCT_OUT)/iago
LOCAL_UNSTRIPPED_PATH := $(PRODUCT_OUT)/iago/debug

include $(BUILD_EXECUTABLE)

endif # TARGET_USE_IAGO

include $(call all-subdir-makefiles)
