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
#include <gpt/gpt.h>

#include <iago.h>
#include <iago_util.h>

#define BOOTLOADER_PATH			"/mnt/bootloader/"
#define SYSLINUX_BIN			"/installmedia/images/android_syslinux"
#define SYSLINUX_CFG_TEM_FN		"/installmedia/images/syslinux.template.cfg"
#define SYSLINUX_MBR			"/installmedia/images/gptmbr.bin"
#define SYSLINUX_CFG_FN			BOOTLOADER_PATH "/syslinux.cfg"
#define IMAGES_PATH			"/installmedia/images/"

static void do_install_syslinux(const char *device)
{
	int rv = -1;

	/* executing syslinux to installer bootloader */
	if ((rv = execute_command(SYSLINUX_BIN " --install %s", device)))
		die("Error while running syslinux: %d", rv);
}


void syslinux_cli(void)
{
	/* If some other bootloader already specified, bail */
	if (strcmp(hashmapGetPrintf(ictx.opts, "none", BASE_BOOTLOADER), "none"))
		return;

	if (!ui_ask("Install SYSLINUX bootloader? (legacy mode, UNSUPPORTED)", true))
		return;

	xhashmapPut(ictx.opts, xstrdup(BASE_BOOTLOADER),
			xstrdup("syslinux"));
}


static bool bootimage_cb(char *entry, int index _unused, void *context)
{
	int fd = *((int *)context);
	char *disk, *description, *prefix, *bus;

	prefix = xasprintf("partition.%s", entry);

	description = hashmapGetPrintf(ictx.opts, NULL,
			"%s:description", prefix);
	put_string(fd, "label %s\n", entry);
	put_string(fd, "    menu label ^%s\n", description);
	put_string(fd, "    com32 android.c32\n");
	disk = hashmapGetPrintf(ictx.opts, NULL,
			"%s:index", prefix);
	put_string(fd, "    append current %s", disk);
	bus = hashmapGetPrintf(ictx.opts, NULL, DISK_BUS_NAME);
	if (bus)
		put_string(fd, " androidboot.disk=%s", bus);
	put_string(fd, "\n");
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
	char *device, *diskdevice;
	char *bootimages;
	int fd;

	if (strcmp(hashmapGetPrintf(ictx.opts, "none", BASE_BOOTLOADER),
				"syslinux"))
		return;

	pr_info("Writing MBR");
        diskdevice = xasprintf("/dev/block/%s",
			hashmapGetPrintf(ictx.opts, NULL, BASE_INSTALL_DISK));
	dd(SYSLINUX_MBR, diskdevice);
        free(diskdevice);

	/* SYSLINUX complains if this isn't done */
	chmod("/tmp", 01777);

	bootimages = hashmapGetPrintf(ictx.opts, NULL, BASE_BOOT_LIST);
	device = hashmapGetPrintf(ictx.opts, NULL, "partition.bootloader:device");
	pr_info("Installing ldlinux.sys onto %s", device);
	do_install_syslinux(device);

	/* In case we die() before we are finished */
	signal(SIGABRT, sighandler);
	mount_partition_device(device, "vfat", BOOTLOADER_PATH);

	pr_info("Copying syslinux support files");
	copy_file(IMAGES_PATH "vesamenu.c32", BOOTLOADER_PATH "vesamenu.c32");
	copy_file(IMAGES_PATH "android.c32", BOOTLOADER_PATH "android.c32");

	pr_info("Constructing syslinux.cfg");
	/* Put the initial template stuff in */
	copy_file(SYSLINUX_CFG_TEM_FN, SYSLINUX_CFG_FN);
	fd = xopen(SYSLINUX_CFG_FN, O_WRONLY | O_APPEND);
	put_string(fd, "menu androidcommand %s\n",
		hashmapGetPrintf(ictx.opts, NULL, "partition.misc:index"));
	string_list_iterate(bootimages, bootimage_cb, &fd);

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


