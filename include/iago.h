#ifndef IAGO_H
#define IAGO_H

#include <string.h>
#include <errno.h>

#include <cutils/hashmap.h>
#include <iniparser.h>
#ifndef LOG_TAG
#define LOG_TAG "Iago"
#endif
#include <cutils/log.h>
#include <cutils/hashmap.h>
#include <cutils/list.h>

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#include <selinux/label.h>
#else
struct selabel_handle;
#endif

#include "iago_util.h"

extern struct selabel_handle *sehandle;

struct iago_context {
	/* Configuration parameters supplied at build time
	 * or based on runtime questions */
	Hashmap *opts;

	/* Key/value pairs here will be turned into install.prop
	   at the end of installation */
	Hashmap *iprops;

	/* Key/value pairs here will be added to the kernel command
	 * line in the form key=value. Don't put spaces in the value!!
         * This is used by bootloader and partitioning plugins.
         * Other plugins should not read or write to this */
	Hashmap *cmdline;

	/* Linked list of plug-in modules */
	struct listnode plugins;

	int plugin_count;
};

struct iago_plugin {
	struct listnode entry;

	/* Preparation phase, runs before any interactive input. Intended
	 * for gathering data that requires root since the UI may not have
	 * sufficient permissions. Only modify 'opts'. */
	void (*prepare)(void);

	/* Do a command line interactive session. You can modify ictx.opts
	 * but don't touch the other members */
	void (*cli_session)(void);

	/* Presumes that we have a complete configuration; apply it.
	 * May make changes to ictx.cmdline and ictx.props */
	void (*execute)(void);
};

/* Global installation context that plugins can use and modify */
extern struct iago_context ictx;

#define pr_error(x...) {{ \
    ui_printf("ERROR " x); \
    ALOGE(x); \
}}
#define pr_verbose ALOGV
#define pr_info(x...) {{ \
    ui_printf(x); \
    ALOGI(x); \
}}
#define pr_debug ALOGD
#define pr_perror(x) pr_error("%s: %s\n", x, strerror(errno))

void add_iago_plugin(struct iago_plugin *p);

/* List of partitions to install - each must have a partitions-<name>
 * block in the combined ini */
#define BASE_PTN_LIST		"base:partitions"

/* List of partitions which are boot images, which will be put in
 * the bootloader configuration. First item is the default */
#define BASE_BOOT_LIST		"base:bootimages"

/* Path to device node for installation disk. The bootloader and boot
 * images need to reside here */
#define BASE_INSTALL_DEV	"base:install_device"

/* Name of the bootloader plug-in in use, if any */
#define BASE_BOOTLOADER		"base:bootloader"

/* Partitions to include in fs_mgr fstab */
#define BASE_FSMGR_PTNS		"base:fsmgr_partitions"

#endif
