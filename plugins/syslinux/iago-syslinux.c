/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	 http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

#include <cutils/config_utils.h>
#include <cutils/misc.h>

#include <iago.h>
#include <iago_util.h>

#define BOOTLOADER_PATH			"/mnt/bootloader"
#define SYSLINUX_BIN			"/installmedia/images/android_syslinux"
#define SYSLINUX_FILES_PATH		"/installmedia/images/syslinux"
#define SYSLINUX_CFG_TEM_FN		"/installmedia/images/syslinux.template.cfg"
#define SYSLINUX_MBR			"/installmedia/images/gptmbr.bin"
#define SYSLINUX_CFG_FN			BOOTLOADER_PATH "/syslinux.cfg"


static void do_install_syslinux(const char *device)
{
	int rv = -1;

	/* executing syslinux to installer bootloader */
	if ((rv = execute_command(SYSLINUX_BIN " --install %s", device)))
		die("Error while running syslinux: %d", rv);
}


static void do_copy_syslinux_files()
{
	DIR* dirp;
	struct dirent *de;
	char *src, *dest;

	/* Copy files there */
	dirp = opendir(SYSLINUX_FILES_PATH);
	if (!dirp)
		die_errno("opendir");

	while ( (de = readdir(dirp)) ) {
		if (de->d_type != DT_REG)
			continue;

		src = xasprintf("%s/%s", SYSLINUX_FILES_PATH, de->d_name);
		dest = xasprintf("%s/%s", BOOTLOADER_PATH, de->d_name);
		copy_file(src, dest);
		free(src);
		free(dest);
	}
	closedir(dirp);
}


void syslinux_cli(void)
{
	// ask: do we want to install a bootloader?
	char *plist;
	plist = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);
	string_list_append(&plist, "bootloader");
	xhashmapPut(ictx.opts, xstrdup(BASE_PTN_LIST), plist);
	xhashmapPut(ictx.opts, xstrdup(BASE_BOOTLOADER),
			xstrdup("syslinux"));
}

static bool cmdline_cb(void *k, void *v, void *context)
{
	int fd = (int)context;
	char *key = k;
	char *value = v;

	put_string(fd, " %s=%s", key, value);
	return true;
}


static bool bootimage_cb(char *entry, int index _unused, void *context)
{
	int fd = (int)context;
	char *disk, *description, *prefix;

	prefix = xasprintf("partition.%s", entry);

	description = hashmapGetPrintf(ictx.opts, NULL,
			"%s:description", prefix);
	put_string(fd, "label %s\n", entry);
	put_string(fd, "    menu label ^%s\n", description);
	put_string(fd, "    com32 android.c32\n");
#if SYSLINUX_USE_GUID
	put_string(fd, "    append current GUID=%s",
			hashmapGetPrintf(ictx.opts, NULL,
			"%s:guid", prefix));
#else
	disk = hashmapGetPrintf(ictx.opts, NULL,
			"%s:index", prefix);
	put_string(fd, "    append current %s", disk);
#endif

	hashmapForEach(ictx.cmdline, cmdline_cb, (void*)fd);

	put_string(fd, "\n\n");
	return true;
}


static void sighandler(int signum)
{
	umount(BOOTLOADER_PATH);
	rmdir(BOOTLOADER_PATH);
	raise(signum);
}


void syslinux_execute(void)
{
	char *device;
	char *bootimages;
	int fd;

	if (strcmp(hashmapGetPrintf(ictx.opts, "none", BASE_BOOTLOADER),
				"syslinux"))
		return;

	pr_info("Writing MBR");
	dd(SYSLINUX_MBR, hashmapGetPrintf(ictx.opts, NULL,
				BASE_INSTALL_DEV));

	/* SYSLINUX complains if this isn't done */
	chmod("/tmp", 01777);

	bootimages = hashmapGetPrintf(ictx.opts, NULL, BASE_BOOT_LIST);
	device = hashmapGetPrintf(ictx.opts, NULL, "partition.bootloader:device");
	pr_info("Installing ldlinux.sys");
	do_install_syslinux(device);

	/* In case we die() before we are finished */
	signal(SIGABRT, sighandler);
	mount_partition_device(device, "vfat", BOOTLOADER_PATH);
	pr_info("Copying syslinux support files");
	do_copy_syslinux_files();

	pr_info("Constructing syslinux.cfg");
	/* Put the initial template stuff in */
	copy_file(SYSLINUX_CFG_TEM_FN, SYSLINUX_CFG_FN);
	fd = xopen(SYSLINUX_CFG_FN, O_WRONLY | O_APPEND);

#if SYSLINUX_USE_GUID
	put_string(fd, "menu androidcommand GUID=%s\n", device_to_uuid(
				hashmapGetPrintf(ictx.opts, NULL,
					         "partition.misc:device")));
#else
	put_string(fd, "menu androidcommand %s\n",
		hashmapGetPrintf(ictx.opts, NULL, "partition.misc:index"));
#endif
	string_list_iterate(bootimages, bootimage_cb, (void*)fd);

	xclose(fd);
	umount(BOOTLOADER_PATH);
	rmdir(BOOTLOADER_PATH);
	signal(SIGABRT, SIG_DFL);
	pr_info("SYSLINUX installation complete");
}


static struct iago_plugin plugin = {
	.cli_session = syslinux_cli,
	.execute = syslinux_execute
};


struct iago_plugin *syslinux_init(void)
{
	return &plugin;
}


