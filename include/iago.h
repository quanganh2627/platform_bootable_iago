#ifndef IAGO_H
#define IAGO_H

#include <string.h>
#include <errno.h>

#include <cutils/hashmap.h>
#ifndef LOG_TAG
#define LOG_TAG "Iago"
#endif
#include <cutils/log.h>
#include <cutils/hashmap.h>
#include <cutils/list.h>
#include <cutils/klog.h>

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#include <selinux/label.h>
#else
struct selabel_handle;
#endif

extern struct selabel_handle *sehandle;

struct iago_context {
	/* Configuration parameters supplied at build time
	 * or based on runtime questions */
	Hashmap *opts;

	/* Key/value pairs here will be turned into install.prop
	 * at the end of installation */
	Hashmap *iprops;

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

struct ui_option {
	/* Embedded list strict */
	struct listnode list;

	/* String option to be selected; returned by selection function */
	char *option;

	/* Description to show to the user */
	char *description;
};

/* Global installation context that plugins can use and modify */
extern struct iago_context ictx;

#define pr_error(x...) do {\
    ui_printf(UI_PRINT_ERROR, x); \
} while(0)
#define pr_verbose do {\
    ui_printf(UI_PRINT_VERBOSE, x); \
} while(0)
#define pr_info(x...) do {\
    ui_printf(UI_PRINT_INFO, x); \
} while(0)
#define pr_debug(x...) do {\
    ui_printf(UI_PRINT_DEBUG, x); \
} while(0)
#define pr_perror(x) pr_error("%s: %s\n", x, strerror(errno))

void add_iago_plugin(struct iago_plugin *p);

#define COMBINED_INI		"/data/iago.ini"

/* List of partitions to install - each must have a partitions-<name>
 * block in the combined ini */
#define BASE_PTN_LIST		"base:partitions"

/* List of partitions which are boot images, which will be put in
 * the bootloader configuration. First item is the default */
#define BASE_BOOT_LIST		"base:bootimages"

/* List of all available disks to install Android on. Each will have
 * a corresponding disk.<name> block with keys sectors, lba_size, size,
 * and model */
#define BASE_DISK_LIST		"base:disks"

/* Name of the installation disk for referencing disk.XX config entrie.
 * Name of the device node without path information */
#define BASE_INSTALL_DISK	"base:install_disk"

/* Nonzero if we are installing in a dual boot configuration */
#define BASE_DUAL_BOOT		"base:dualboot"

/* Name of the bootloader plug-in in use, if any */
#define BASE_BOOTLOADER		"base:bootloader"

/* Reboot target after installation is complete, default just boot normally */
#define BASE_REBOOT		"base:reboot_target"

/* Nonzero if an interactive session */
#define BASE_INTERACTIVE	"base:interactive_mode"

/* Detected bus controller, for by-name symlinks. Should set
 * androidboot.disk to this value */
#define DISK_BUS_NAME		"base:disk_bus"

#endif
