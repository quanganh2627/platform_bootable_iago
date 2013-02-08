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
#define IMAGES_PATH			"/installmedia/images/"
#define EFI_ENTRY			"Android-IA"
#define GUMMIBOOT_TIMEOUT		"gummiboot:timeout"
#define TIMEOUT_DFL			"5"

void gummiboot_cli(void)
{
	unsigned int timeout;

	// TODO skip if BASE_BOOTLOADER is set already

	if (!ui_ask("Install GummiBoot bootloader?", true))
		return;

	xhashmapPut(ictx.opts, xstrdup(BASE_BOOTLOADER),
			xstrdup("gummiboot"));
	timeout = ui_get_value("Enter boot menu timeout (0=no menu)",
			xatoll(TIMEOUT_DFL), 0, 60);
	xhashmapPut(ictx.opts, xstrdup(GUMMIBOOT_TIMEOUT),
			xasprintf("%u", timeout));
}


static void gummiboot_prepare(void)
{
}


static bool cmdline_cb(void *k, void *v, void *context)
{
	int fd = (int)context;
	char *key = k;
	char *value = v;

	put_string(fd, " %s=%s", key, value);
	return true;
}


static bool bootimage_cb(char *entry, int index _unused, void *context _unused)
{
	int fd;
	char *filename;
	char *description, *prefix, *guid;

	filename = xasprintf(BOOTLOADER_PATH "/loader/entries/%s.conf", entry);
	fd = xopen(filename, O_WRONLY | O_CREAT);
	free(filename);
	prefix = xasprintf("partition.%s", entry);

	description = hashmapGetPrintf(ictx.opts, NULL,
			"%s:description", prefix);
	put_string(fd, "title %s\n", description);
	guid = hashmapGetPrintf(ictx.opts, NULL, "%s:guid", prefix);
	put_string(fd, "android %s\n", guid);
	put_string(fd, "options ");

	hashmapForEach(ictx.cmdline, cmdline_cb, (void*)fd);

	put_string(fd, "\n\n");
	xclose(fd);
	free(prefix);
	return true;
}

static void sighandler(int signum)
{
	umount(BOOTLOADER_PATH);
	rmdir(BOOTLOADER_PATH);
	raise(signum);
}

static void gummiboot_execute(void)
{
	char *device;
	char *bootimages;
	int fd, ret;

	if (strcmp(hashmapGetPrintf(ictx.opts, "none", BASE_BOOTLOADER),
				"gummiboot"))
		return;

	bootimages = hashmapGetPrintf(ictx.opts, NULL, BASE_BOOT_LIST);
	device = hashmapGetPrintf(ictx.opts, NULL, "partition.bootloader:device");

	/* In case we die() before we are finished */
	signal(SIGABRT, sighandler);

	mount_partition_device(device, "vfat", BOOTLOADER_PATH);
	pr_info("Copying gummiboot support files");
	copy_file(IMAGES_PATH "/gummiboot.efi", BOOTLOADER_PATH "/gummiboot.efi");

	// TODO recursively delete loader directory if it already exists

	xmkdir(BOOTLOADER_PATH "/loader", 0777);
	fd = xopen(BOOTLOADER_PATH "/loader/loader.conf", O_WRONLY | O_CREAT);
	put_string(fd, "timeout %s\n", hashmapGetPrintf(ictx.opts, TIMEOUT_DFL, GUMMIBOOT_TIMEOUT));
	put_string(fd, "default boot\n");
	put_string(fd, "android-bcb %s\n", hashmapGetPrintf(ictx.opts, NULL, "partition.misc:guid"));
	xclose(fd);

	xmkdir(BOOTLOADER_PATH "/loader/entries", 0777);
	pr_info("Constructing loader entries");
	string_list_iterate(bootimages, bootimage_cb, (void*)fd);

	ret = execute_command("efibootmgr -c -d /dev/block/%s -l \\\\gummiboot.efi -v -p %s -D %s -L %s",
			hashmapGetPrintf(ictx.opts, NULL, BASE_INSTALL_DISK),
			hashmapGetPrintf(ictx.opts, NULL, "partition.bootloader:index"),
			EFI_ENTRY, EFI_ENTRY);
	if (ret)
		die("Some problem with efibootmgr");

	umount(BOOTLOADER_PATH);
	rmdir(BOOTLOADER_PATH);
	signal(SIGABRT, SIG_DFL);
	pr_info("Gummiboot installation complete");
}


static struct iago_plugin plugin = {
	.cli_session = gummiboot_cli,
	.execute = gummiboot_execute,
	.prepare = gummiboot_prepare
};


struct iago_plugin *gummiboot_init(void)
{
	return &plugin;
}


