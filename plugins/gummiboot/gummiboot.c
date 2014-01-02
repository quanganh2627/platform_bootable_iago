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
#include <ftw.h>

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
	struct stat sb;

	/* If some other bootloader already specified, bail */
	if (strcmp(hashmapGetPrintf(ictx.opts, "none", BASE_BOOTLOADER), "none"))
		return;

	/* Check if booted in legacy mode */
	if (stat("/sys/firmware/efi", &sb))
		return;

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


static bool bootimage_cb(char *entry, int index _unused, void *context _unused)
{
	int fd;
	char *filename, *bus;
	char *description, *prefix, *guid;

	filename = xasprintf(BOOTLOADER_PATH "/loader/entries/%s.conf", entry);
	fd = xopen(filename, O_WRONLY | O_CREAT | O_TRUNC);
	free(filename);
	prefix = xasprintf("partition.%s", entry);
	description = hashmapGetPrintf(ictx.opts, NULL,
			"%s:description", prefix);
	put_string(fd, "title %s\n", description);
	guid = hashmapGetPrintf(ictx.opts, NULL, "%s:guid", prefix);
	put_string(fd, "android %s\n", guid);
	bus = hashmapGetPrintf(ictx.opts, NULL, DISK_BUS_NAME);
	if (bus)
		put_string(fd, "android-bus %s\n", bus);
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


int delete_cb(const char *fpath, const struct stat *sb _unused,
		int typeflag _unused, struct FTW *ftwbuf _unused)
{
	remove(fpath);
	return 0;
}


void clean_pstore(void)
{
	/* If there is too much crash dump junk in the pstore,
	 * efibootmgr will fail */
	xmkdir("/pstore", 0777);
	if (mount("pstore", "/pstore", "pstore", 0, NULL)) {
		pr_info("Couldn't mount pstore, is CONFIG_PSTORE enabled?");
		return;
	}
	nftw("/pstore", delete_cb, 64, FTW_DEPTH | FTW_PHYS);
	umount("/pstore");
}


static void gummiboot_execute(void)
{
	char *device;
	char *bootimages;
	char *fallback_efi;
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
	copy_file(IMAGES_PATH "/shim.efi", BOOTLOADER_PATH "/shim.efi");
#ifdef USE_MOKMANAGER
	copy_file(IMAGES_PATH "/MokManager.efi", BOOTLOADER_PATH "/MokManager.efi");
#endif
	xmkdir(BOOTLOADER_PATH "/EFI", 0777);
	xmkdir(BOOTLOADER_PATH "/EFI/Boot", 0777);
	fallback_efi = xasprintf(BOOTLOADER_PATH "/EFI/Boot/%s",
			strcmp(KERNEL_ARCH, "x86_64") ? "bootia32.efi" : "bootx64.efi");
	copy_file(IMAGES_PATH "/shim.efi", fallback_efi);
	free(fallback_efi);
	nftw(BOOTLOADER_PATH "/loader", delete_cb, 64, FTW_DEPTH | FTW_PHYS);
	xmkdir(BOOTLOADER_PATH "/loader", 0777);
	fd = xopen(BOOTLOADER_PATH "/loader/loader.conf", O_WRONLY | O_CREAT);
	put_string(fd, "timeout %s\n", hashmapGetPrintf(ictx.opts, TIMEOUT_DFL, GUMMIBOOT_TIMEOUT));
	put_string(fd, "default boot\n");
	put_string(fd, "android-bcb %s\n", hashmapGetPrintf(ictx.opts, NULL, "partition.misc:guid"));
	xclose(fd);

	xmkdir(BOOTLOADER_PATH "/loader/entries", 0777);
	pr_info("Constructing loader entries");
	string_list_iterate(bootimages, bootimage_cb, NULL);

	device = xasprintf("/dev/block/%s", hashmapGetPrintf(ictx.opts, NULL, BASE_INSTALL_DISK));
	clean_pstore();
	ret = execute_command_no_shell("/sbin/efibootmgr",
			"efibootmgr", "-c", "-d", device, "-l", "\\shim.efi",
			"-v", "-p", hashmapGetPrintf(ictx.opts, NULL, "partition.bootloader:index"),
			"-D", EFI_ENTRY, "-L", EFI_ENTRY, NULL);
	free(device);
	if (ret)
		die("'efibootmgr' encountered an error (status %x)", ret);

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


