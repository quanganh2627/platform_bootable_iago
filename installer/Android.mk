LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := main.c \
		   util.c \
		   partitioner.c \
		   finalizer.c \
		   imagewriter.c \


LOCAL_CFLAGS := -DDEVICE_NAME=\"$(TARGET_BOOTLOADER_BOARD_NAME)\" \
	-W -Wall -Werror -O0

plugin_names := $(foreach plugin,$(TARGET_IAGO_PLUGINS),$(notdir $(plugin)))
plugin_lib_names := $(foreach plugin,$(TARGET_IAGO_PLUGINS),libiago_$(notdir $(plugin)))

LOCAL_MODULE := iagod
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libcutils \
			  liblog \
			  libext4_utils \
			  libz \

LOCAL_STATIC_LIBRARIES := libiniparser \
			  $(plugin_lib_names) \
			  $(TARGET_IAGO_EXTRA_LIBS) \

LOCAL_C_INCLUDES += external/zlib \
		    external/iniparser/src \
		    system/extras/ext4_utils \
		    $(LOCAL_PATH)/../include \

LOCAL_MODULE_PATH := $(PRODUCT_OUT)/iago
LOCAL_UNSTRIPPED_PATH := $(PRODUCT_OUT)/iago/debug

# TARGET_IAGO_PLUGINS is a listing of directories containing IAGO plugins
# that we want to use, defined somewhere in BoardConfig.mk. Each plug-in
# directory must contain the following items:
#
# 1. image.mk - Makefile which can modify IAGO_IMAGES_DEPS, which is a list
# of files be included in the images filesystem. See base image.mk in toplevel.
#
# 2. The plug-in library itself, which has a special name; if the plugin
# directory name is foo, the library is named libiago_foo
#
# 3. iago.ini - Additional build-time configuration directives which gets
# appended to the base installer/iago.ini

# Each library in should have a function named "<pluginname>_init()".
# This returns an newly allocated struct iago_plugin. Here we emit a
# little C function that gets #included by the installer.  It calls all
# those registration functions.

# Devices can also add libraries to TARGET_IAGO_EXTRA_LIBS.
# These libs are also linked in with iagod, but we don't try to call
# any sort of registration function for these.  Use this variable for
# any subsidiary static libraries required for your registered
# plugin libs.

inc := $(call intermediates-dir-for,PACKAGING,iago_extensions)/register.inc

# During the first pass of reading the makefiles, we dump the list of
# extension libs to a temp file, then copy that to the ".list" file if
# it is different than the existing .list (if any).  The register.inc
# file then uses the .list as a prerequisite, so it is only rebuilt
# (and aboot.o recompiled) when the list of extension libs changes.

junk := $(shell mkdir -p $(dir $(inc));\
	        echo $(TARGET_IAGO_PLUGINS) > $(inc).temp;\
	        diff -q $(inc).temp $(inc).list 2>/dev/null || cp -f $(inc).temp $(inc).list)

$(inc) : libs := $(plugin_names)
$(inc) : $(inc).list $(LOCAL_PATH)/Android.mk
	$(hide) mkdir -p $(dir $@)
	$(hide) echo "" > $@
	$(hide) $(foreach lib,$(libs), echo -e "extern struct iago_plugin *$(lib)_init(void);\n" >> $@;)
	$(hide) echo "void register_iago_plugins() {" >> $@
	$(hide) $(foreach lib,$(libs), echo "  add_iago_plugin($(lib)_init());" >> $@;)
	$(hide) echo "}" >> $@

$(call intermediates-dir-for,EXECUTABLES,iagod)/main.o : $(inc)
LOCAL_C_INCLUDES += $(dir $(inc))

include $(BUILD_EXECUTABLE)

