/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#define _BSD_SOURCE

#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fs.h>
#include <regex.h>

#include <cutils/properties.h>
#include <gpt/gpt.h>

#include <iago.h>
#include <iago_util.h>

#include "iago_private.h"

#define NAME_MAGIC	"ANDROID!"
#define MIN_DATA_PART_SIZE	350 /* CDD section 7.6.1 */


static uint64_t round_up_to_multiple(uint64_t val, uint64_t multiple)
{
	uint64_t rem;
	if (!multiple)
		return val;
	rem = val % multiple;
	if (!rem)
		return val;
	else
		return val + multiple - rem;
}


static uint64_t mib_align(uint64_t val)
{
	return round_up_to_multiple(val, 1 << 20);
}


static uint64_t to_unit_ceiling(uint64_t val, uint64_t unit)
{
	return round_up_to_multiple(val, unit) / unit;
}


static uint64_t to_mib(uint64_t val)
{
	return to_unit_ceiling(val, 1 << 20);
}


static uint64_t to_mib_floor(uint64_t val)
{
	return val >> 20;
}


static char *get_install_id(void)
{
	char buf[PROPERTY_VALUE_MAX];
	int ret;

	ret = property_get("ro.boot.install_id", buf, NULL);
	if (ret <= 0)
		return NULL;
	return xstrdup(buf);
}


static uint64_t mib_to_lba(struct gpt *gpt, uint64_t mib)
{
	return (mib << 20) / gpt->lba_size;
}


static void set_install_id(void)
{
	int fd;
	char *install_id_str = get_install_id();

	if (!install_id_str) {
		uint32_t install_id;
		fd = xopen("/dev/urandom", O_RDONLY);
		xread(fd, &install_id, sizeof(install_id));
		xclose(fd);
		install_id_str = xasprintf("%s%08X", NAME_MAGIC, install_id);
		if (property_set("ro.boot.install_id", install_id_str))
			die("Unable to set ro.boot.install_id");
	}
	xhashmapPut(ictx.opts, xstrdup(INSTALL_ID),
			install_id_str);
}


/* Examine the disk for a partition that has the specified type
 * GUID. Return the index of the first one found */
static int check_for_ptn(struct gpt *gpt, const struct guid *guid)
{
	uint32_t i;
	struct gpt_entry *e;

	partition_for_each(gpt, i, e) {
		if (!memcmp(guid, &(e->type_guid), sizeof(struct guid))) {
			return i;
		}
	}
	return -1;
}


/* Examine the disk for an existing Android installation,
 * returning the number of bytes it is taking up on the disk
 * to show how much space can be freed. The ESP is not counted
 * since it wouldn't be deleted */
static uint64_t check_for_android(struct gpt *gpt)
{
	uint32_t i;
	struct gpt_entry *e;
	uint64_t size;

	size = 0;
	partition_for_each(gpt, i, e) {
		char *name = gpt_entry_get_name(e);
		bool c = false;
		if (strncmp(name, NAME_MAGIC, 8))
			c = true;
		if (strlen(name) == 26 && !strcmp(name + 16, "bootloader"))
			c = true; /* skip the ESP */
		free(name);
		if (c)
			continue;
		size += gpt_entry_get_size(gpt, e);
	}
	return size;
}




/* Examine the partition entry at the specified index to see if
 * it is an NTFS volume that can be resized.
 *
 * Returns positive value in bytes for the smallest size the partition
 * can be resized to.
 * On errors, returns:
 *    -EINVAL - Not an NTFS volume
 *    -EROFS  - NTFS volume cannot be resized; run chkdsk in Windows
 *    -EIO    - Can't read the volume, possibly corrupt?
 */
static int64_t get_ntfs_min_size(int index, struct gpt *gpt)
{
	struct gpt_entry *e;
	char *device;
	int ret;
	char buf[4096];
	char *pos, *pos2;
	size_t sz;

	static const char *needle = "You might resize at ";

	e = gpt_entry_get(index, gpt);
	if (guidcmp(&e->type_guid, get_guid_type(PART_MS_DATA)))
		return -EINVAL;

	device = gpt_get_device_node(index, gpt);
	if (!device)
		die("gpt_get_device_node");
	ret = execute_command("ntfsresize --check %s", device);
	if (ret) {
		free(device);
		return -EROFS;
	}

	sz = sizeof(buf);
	ret = execute_command_output(buf, &sz,
			"ntfsresize --no-progress-bar --info %s", device);
	free(device);
	if (ret) {
		pr_info("%s", buf);
		return -EIO;
	}
	pos = strstr(buf, needle);
	if (!pos)
		die("ntfsresize returned '%s'", buf);
	pos += strlen(needle);
	pos2 = strstr(pos, " ");
	*pos2 = '\0';

	return xatoll(pos);
}


/* Resize an NTFS partition to the new size in bytes. Does very little error checking;
 * always run get_ntfs_min_size() first. */
static void resize_ntfs_partition(int index, struct gpt *gpt, uint64_t new_size)
{
	struct gpt_entry *e;
	int ret;
	char *device;

	/* Assumes all checks in get_ntfs_min_size_mb have been done */
	e = gpt_entry_get(index, gpt);

	/* Resize the NTFS filesystem */
	device = gpt_get_device_node(index, gpt);
	if (!device)
		die("gpt_get_device_node");
	ret = execute_command("ntfsresize --no-action --size %lld %s", new_size, device);
	if (ret)
		die("ntfs resize operation dry run failed");

	ret = execute_command("ntfsresize --force --size %lld %s", new_size, device);
	if (ret)
		die("ntfs resize operation failed. disk is likely corrupted!!");
	free(device);

	/* Now resize the underlying partition */
	e->last_lba = e->first_lba + to_unit_ceiling(new_size, gpt->lba_size) - 1;
}


static void delete_android(struct gpt *gpt)
{
	struct gpt_entry *e;
	uint32_t i;

	partition_for_each(gpt, i, e) {
		char *name;
		bool c = false;

		name = gpt_entry_get_name(e);
		if (!name)
			die("gpt_entry_get_name");

		if (strncmp(name, NAME_MAGIC, 8))
			c = true;
		if (strlen(name) == 26 && !strcmp(name + 16, "bootloader"))
			c = true;
		free(name);
		if (c)
			continue;

		if (gpt_entry_delete(gpt, i))
			die("Couldn't delete partition");
	}
}

/* Used for scanning /sys/block/; reject any matches */
#define DISK_MATCH_REGEX	"^[.]+|(ram|loop|sr)[0-9]+|mmcblk[0-9]+(rpmb|boot[0-9]+)$"

static void partitioner_prepare(void)
{
	/* Scan all the existing available disks and populate opts with their info */
	DIR *dir = NULL;
	char *disks = xstrdup("");
	char media[PROPERTY_VALUE_MAX];
	regex_t diskreg;
	bool interactive;

	property_get("ro.iago.media", media, "");
	interactive = atoi(hashmapGetPrintf(ictx.opts, "0", BASE_INTERACTIVE));

	dir = opendir("/sys/block");
	if (!dir)
		die();

	if (regcomp(&diskreg, DISK_MATCH_REGEX, REG_EXTENDED | REG_NOSUB))
		die_errno("regcomp");

	while (1) {
		struct dirent *dp = readdir(dir);
		uint64_t sectors, lba_size;
		uint64_t android_size;
		uint64_t free_start_lba, free_end_lba;
		struct gpt *gpt;
		char *device;
		int ptn_index;
		char *diskname_node;
		struct stat sb;

		if (!dp)
			break;

		if (!regexec(&diskreg, dp->d_name, 0, NULL, 0)) {
			pr_debug("Skipping %s\n", dp->d_name);
			continue;
		}

		if (!strcmp(dp->d_name, media)) {
			pr_debug("Skipping Iago media %s\n", dp->d_name);
			continue;
		}

		device = xasprintf("/dev/block/%s", dp->d_name);
		sectors = read_sysfs_int("/sys/block/%s/size", dp->d_name);
		lba_size = read_sysfs_int("/sys/block/%s/queue/logical_block_size",
				dp->d_name);

		xhashmapPut(ictx.opts, xasprintf("disk.%s:sectors", dp->d_name),
				xasprintf("%llu", sectors));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:lba_size", dp->d_name),
				xasprintf("%llu", lba_size));
		diskname_node = xasprintf("/sys/block/%s/device/model", dp->d_name);
		if (stat(diskname_node, &sb)) {
			free(diskname_node);
			diskname_node = xasprintf("/sys/block/%s/device/name", dp->d_name);
		}
		xhashmapPut(ictx.opts, xasprintf("disk.%s:size", dp->d_name),
				xasprintf("%llu", lba_size * sectors));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:model", dp->d_name),
				read_sysfs("%s", diskname_node));
		free(diskname_node);
		xhashmapPut(ictx.opts, xasprintf("disk.%s:device", dp->d_name),
				device);
		string_list_append(&disks, dp->d_name);

		gpt = gpt_init(device);
		if (!gpt)
			die("gpt allocation");
		if (gpt_read(gpt)) {
			gpt_close(gpt);
			continue;
		}

		ptn_index = check_for_ptn(gpt, get_guid_type(PART_MS_RESERVED));
		if (ptn_index > 0) {
			/* User data NTFS partition always right after
			 * the ms reserved partition */
			uint64_t ret, size;
			int ms_data_idx = ptn_index + 1;
			size = gpt_entry_get_size(gpt, gpt_entry_get(ms_data_idx, gpt));
			pr_info("Disk %s has an existing Windows installation",
					dp->d_name);
			pr_debug("%s: Found Windows data at partition index %d of size %llu MiB\n",
					dp->d_name, ms_data_idx, to_mib(size));
			xhashmapPut(ictx.opts,
					xasprintf("disk.%s:msdata_index", dp->d_name),
					xasprintf("%d", ms_data_idx));
			xhashmapPut(ictx.opts,
					xasprintf("disk.%s:msdata_size", dp->d_name),
					xasprintf("%llu", size));
			if (interactive) {
				ret = get_ntfs_min_size(ms_data_idx, gpt);
				switch (ret) {
				case -EINVAL:
				case -EIO:
					pr_debug("%s: MS Data partition unreadable",
							dp->d_name);
					break;
				case -EROFS:
					pr_debug("%s: MS Data partition not resizeable",
							dp->d_name);
					// todo communicate this formally to user
					break;
				default:
					pr_debug("%s: MS Data partition resizable to %lld MiB",
							dp->d_name, to_mib(ret));
					xhashmapPut(ictx.opts,
						xasprintf("disk.%s:msdata_minsize", dp->d_name),
						xasprintf("%llu", ret));
				}
			}
		} else
			pr_debug("%s: No Windows installation found\n", dp->d_name);

		ptn_index = check_for_ptn(gpt, get_guid_type(PART_ESP));
		if (ptn_index > 0) {
			/* We found an EFI system partition and may
			 * re-use it */
			pr_debug("%s: Found ESP at partition index %d\n",
					dp->d_name, ptn_index);
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:esp_index", dp->d_name),
				xasprintf("%d", ptn_index));
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:esp_size", dp->d_name),
				xasprintf("%llu", mib_align(gpt_entry_get_size(
							gpt, gpt_entry_get(ptn_index, gpt)))));
		} else
			pr_debug("%s: no EFI System Partition found\n",
					dp->d_name);
		android_size = check_for_android(gpt);
		if (android_size) {
			pr_info("Disk %s has an existing Android installation",
					dp->d_name);
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:android_size", dp->d_name),
				xasprintf("%llu", android_size));
		}
		if (!gpt_find_contiguous_free_space(gpt, &free_start_lba, &free_end_lba)) {
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:free_size", dp->d_name),
				xasprintf("%llu", ((free_end_lba + 1) - free_start_lba)
					* gpt->lba_size));
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:free_start_lba", dp->d_name),
				xasprintf("%llu", free_start_lba));
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:free_end_lba", dp->d_name),
				xasprintf("%llu", free_end_lba));
		}
		gpt_close(gpt);
	}
	closedir(dir);

	if (strlen(disks) == 0)
		die("No suitable installation media found!");

	xhashmapPut(ictx.opts, xasprintf(BASE_DISK_LIST), disks);
}


static bool disk_list_cb(char *disk, int _unused index, void *context)
{
	struct listnode *disk_list = context;
	struct ui_option *opt = xmalloc(sizeof(*opt));
	opt->option = xstrdup(disk);
	opt->description = xasprintf("%9s %9lldM '%s'", disk,
		xatoll(hashmapGetPrintf(ictx.opts, NULL, "disk.%s:size", disk)),
		hashmapGetPrintf(ictx.opts, NULL, "disk.%s:model", disk));
	list_add_tail(disk_list, &opt->list);
	return true;
}


static bool sumsizes_cb(char *entry, int index _unused, void *context)
{
	uint64_t *total = context;
	int64_t len;

	if (!strncmp(entry, "bootloader", 10))
		return true;

	len = xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.%s:len", entry));
	if (len > 0)
		*total += (len << 20);
	return true;
}


/* Calculate the disk space required for the Android installation.
 * esp_sizes is addional esp_space needed. If we're doing dual boot
 * and preserving the existing ESP, this is the size for the backup ESP
 * partition. Otherwise, its the combined size of both. Returned value
 * does not include the data partition; add MIN_DATA_PART_SIZE if you
 * need that too.*/
static uint64_t get_partial_space_required(uint64_t esp_sizes)
{
	char *partlist;
	uint64_t total = 0;

	partlist = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);
	/* sum up all non-bootloader partitions */
	string_list_iterate(partlist, sumsizes_cb, &total);
	total += esp_sizes; /* for the copy */
	return total;
}


static uint64_t get_all_space_required(char *disk, bool dualboot)
{
	uint64_t bootloader_size;

	if (dualboot) {
		/* Space for backup copy of existing ESP already on disk */
		bootloader_size = xatol(hashmapGetPrintf(ictx.opts, "0",
					"disk.%s:esp_size", disk));
	} else {
		bootloader_size = (xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.bootloader:len")) << 20) * 2;
	}
	return get_partial_space_required(bootloader_size) + (MIN_DATA_PART_SIZE << 20);
}


static void partitioner_cli(void)
{
	char *disk;
	int64_t android_size, windows_size, windows_min_size, free_size;
	int64_t windows_max_size, total_free_size, required_size;
	int64_t disk_size, new_windows_size;

	bool can_resize = false;

	list_declare(disk_list);

	string_list_iterate(hashmapGetPrintf(ictx.opts, NULL, BASE_DISK_LIST),
			disk_list_cb, &disk_list);

	disk = xstrdup(ui_option_get("Choose disk to install Android:",
					&disk_list));
	xhashmapPut(ictx.opts, xstrdup(BASE_INSTALL_DISK),
				xstrdup(disk));
	option_list_free(&disk_list);

	windows_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:msdata_size", disk));
	windows_min_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:msdata_minsize", disk));
	disk_size = xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"disk.%s:size", disk));
	android_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:android_size", disk));
	free_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:free_size", disk));

	if (android_size)
		pr_info("Android installation detected. It will be deleted, freeing %llu MiB",
				to_mib_floor(android_size));
	if (free_size)
		pr_info("Unpartitioned space: %llu MiB",
				to_mib_floor(free_size));

	if (windows_size) {
		pr_info("Windows data partition detected of size %llu MiB.",
				to_mib(windows_size));
		if (windows_min_size) {
			pr_info("Windows data partition can be shrunk to as low as %llu MiB.",
					to_mib(windows_min_size));
			can_resize = true;
		} else {
			pr_info("However, you need to reboot into Windows and scan the drive for errors, it cannot be resized right now.");
		}

		pr_info("IMPORTANT: Back up ALL data before installing a dual boot configuration");
		if (ui_ask("Do you want to preserve Windows and dual boot?", true)) {
			bool must_resize = false;

			/* Assumes if Android is present, there is no
			 * additional free space on the disk. Reasonable given
			 * how /data is expanded. We should still add an
			 * advanced partitioning option which drops into
			 * an internactive partition editing session */
			if (android_size)
				total_free_size = android_size;
			else
				total_free_size = free_size;
			if (android_size && to_mib_floor(free_size) != 0)
				pr_info("Your disk is in a weird state, with extra free space. Consider manually repartitioning");

			required_size = get_all_space_required(disk, true);
			pr_info("Total available free space is %llu MiB. We require at least %llu MiB.",
					to_mib_floor(total_free_size), to_mib(required_size));
			if (required_size > total_free_size) {
				windows_max_size = windows_size - (required_size - total_free_size);
				must_resize = true;
				if (windows_max_size < 0 ||
						windows_max_size < windows_min_size || !can_resize) {
					die("Not enough free room on the disk to install Android, and Windows cannot be shrunk enough to make space");
				}
			} else {
				windows_max_size = windows_size;
			}
			pr_info("NOTE: If you resize Windows, it will undergo a repair cycle the next time it boots. This is normal.");
			if (can_resize && ui_ask("Do you want to resize Windows to make more space?", true)) {
				new_windows_size = ui_get_value("Enter new size in MiB for Windows",
						to_mib_floor(windows_max_size),
						to_mib(windows_min_size),
						to_mib_floor(windows_max_size));
				xhashmapPut(ictx.opts,
						xasprintf("disk.%s:windows_resize", disk),
						xasprintf("%llu", new_windows_size << 20));
			} else if (must_resize) {
				die("Insufficient free space to proceed.");
			}
			xhashmapPut(ictx.opts, xstrdup("base:dualboot"), xstrdup("1"));
			return;
		}
	}

	/* If we get here then there was no Windows partition or we decided not to Dual Boot */
	pr_info("ALL data on the target disk will be erased!");
	if (!ui_ask("Do you want to continue?", false))
		die("Aborted installation due to user request");
	required_size = get_all_space_required(disk, false);
	/* FIXME slightly off as not all of the disk size is usable, but if we don't have
	 * a GPT it's hard to accurately calculate it. Shave off 2 megabytes to be safe */
	disk_size -= (2 * 1024 * 1024);
	if (disk_size < required_size)
		die("Insuffcient disk size (%lld MiB available) to install Android (need %lld MiB)",
			to_mib_floor(disk_size), to_mib(required_size));
}


static char *gpt_name(const char *name)
{
	char *ret, *install_id;

	install_id = get_install_id();
	ret = xasprintf("%s%s", install_id, name);
	free(install_id);
	return ret;
}



static bool flags_cb(char *flag, int _unused index, void *context)
{
	uint64_t *flags_ptr = context;
	uint64_t mask;
	bool enable;

	if (flag[0] == '!') {
		flag++;
		enable = false;
	} else {
		enable = true;
	}

	if (!strcmp(flag, "system"))
		mask = GPT_FLAG_SYSTEM;
	else if (!strcmp(flag, "boot"))
		mask = GPT_FLAG_BOOTABLE;
	else if (!strcmp(flag, "ro"))
		mask = GPT_FLAG_READONLY;
	else if (!strcmp(flag, "hidden"))
		mask = GPT_FLAG_HIDDEN;
	else if (!strcmp(flag, "noauto"))
		mask = GPT_FLAG_NO_AUTOMOUNT;
	else
		die("unknown partition flag '%s'", flag);

	if (enable)
		*flags_ptr |= mask;
	else
		*flags_ptr &= ~mask;

	return true;
}


static enum part_type string_to_type(char *type)
{
	if (!strcmp(type, "esp"))
		return PART_ESP;
	else if (!strcmp(type, "boot"))
		return PART_ANDROID_BOOT;
	else if (!strcmp(type, "misc"))
		return PART_ANDROID_MISC;
	else if (!strcmp(type, "ext4"))
		return PART_LINUX;
	else if (!strcmp(type, "vfat")) {
		return PART_MS_DATA;
	} else
		die("Unknown partition type %s", type);
}


struct mkpart_ctx {
	struct gpt *gpt;

	/* numerical partition index in the GPT; updated with each iteration */
	int ptn_index;

	/* mb offset of the next partition to create */
	uint64_t next_mb;

	/* Size of non-growable partitions */
	uint64_t ptn_mb;

	/* Total available space on the disk */
	uint64_t disk_mb;

	/* Whether to skip processing the 'bootloader' partition */
	bool skip_bootloader;
};


static char *get_device_node(struct gpt *gpt, int index)
{
	char *node;

	node = gpt_get_device_node(index, gpt);
	if (!node)
		die("gpt_get_device_node");
	return node;
}


static bool mkpart_cb(char *entry, int index _unused, void *context)
{
	struct mkpart_ctx *mc = context;
	char *flags_list;
	char *ptype;
	char *name;
	int64_t part_mb;
	uint64_t flags;

	if (mc->skip_bootloader && !strcmp(entry, "bootloader"))
		return true;

	flags_list = hashmapGetPrintf(ictx.opts, "", "partition.%s:flags", entry);
	ptype = hashmapGetPrintf(ictx.opts, "ext4", "partition.%s:type", entry);
	flags = 0;
	string_list_iterate(flags_list, flags_cb, &flags);

	if (strlen(entry) > 27)
		die("Partition '%s' name too long!", entry);

	part_mb = xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.%s:len", entry));
	if (part_mb < 0)
		part_mb = mc->disk_mb - mc->ptn_mb;

	name = gpt_name(entry);

	mc->ptn_index = gpt_entry_create(mc->gpt, name, string_to_type(ptype),
			flags, mib_to_lba(mc->gpt, mc->next_mb),
			mib_to_lba(mc->gpt, mc->next_mb + part_mb) - 1);
	if (mc->ptn_index == 0)
		die("failure creating new partition\n");

	free(name);
	mc->next_mb += part_mb;

	xhashmapPut(ictx.opts, xasprintf("partition.%s:index", entry),
			xasprintf("%d", mc->ptn_index));
	xhashmapPut(ictx.opts, xasprintf("partition.%s:device", entry),
			get_device_node(mc->gpt, mc->ptn_index));
	return true;
}


static void create_android_partitions(uint64_t bootloader_size, char *partlist,
		struct gpt *gpt, bool skip_bootloader)
{
	uint64_t start_lba, end_lba, start_mb, end_mb;
	uint64_t space_needed_mb;
	uint64_t space_available_mb;
	struct mkpart_ctx mc;

	space_needed_mb = to_mib(get_partial_space_required(bootloader_size));

	if (gpt_find_contiguous_free_space(gpt, &start_lba, &end_lba))
		die("Couldn't calculate unpartitioned disk space");

	start_mb = to_mib(start_lba * gpt->lba_size);
	end_mb = to_mib_floor((end_lba + 1) * gpt->lba_size);
	space_available_mb = end_mb - start_mb;
	if (space_available_mb < (space_needed_mb + MIN_DATA_PART_SIZE)) {
		pr_error("Please install interactively to re-partition the disk");
		die("Insufficient disk space (have %llu MiB need %llu MiB)",
				space_available_mb,
				space_needed_mb + MIN_DATA_PART_SIZE);
	}

	/* Always the same size */
	xhashmapPut(ictx.opts, xstrdup("partition.bootloader2:len"),
			xstrdup(hashmapGetPrintf(ictx.opts, NULL,
					"partition.bootloader:len")));

	mc.next_mb = start_mb;
	mc.disk_mb = space_available_mb;
	mc.skip_bootloader = skip_bootloader;
	mc.ptn_mb = space_needed_mb;
	mc.gpt = gpt;

	pr_debug("offset=%llu space_available=%llu space_needed=%llu",
			mc.next_mb, mc.disk_mb, mc.ptn_mb);
	string_list_iterate(partlist, mkpart_cb, &mc);
}


struct gpt *execute_dual_boot(char *disk, char *partlist, char *device)
{
	uint64_t esp_size, win_resize;
	uint32_t esp_index, win_index;
	char *name;
	struct gpt *gpt;
	struct gpt_entry *esp;

	gpt = gpt_init(device);
	if (!gpt)
		die("gpt_init");

	win_resize = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:windows_resize", disk));
	esp_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:esp_size", disk));
	esp_index = xatol(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:esp_index", disk));
	win_index = xatol(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:msdata_index", disk));

	if (!esp_index) {
		pr_info("Existing EFI system partition not found.");
		die("Please use the interactive installer to re-partition the disk.");
	}

	if (gpt_read(gpt))
		die("Couldn't read existing GPT.");

	if (win_resize) {
		pr_info("Resizing Windows partition");
		resize_ntfs_partition(win_index, gpt, win_resize);
	}

	if (win_index)
		xhashmapPut(ictx.iprops, xstrdup("ro.rtc_local_time"), "1");

	if (xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:android_size", disk))) {
		pr_info("Deleting existing Android installation");
		delete_android(gpt);
	}

	/* Claim existing ESP as our own bootloader partition */
	xhashmapPut(ictx.opts, xstrdup("partition.bootloader:mode"),
			xstrdup("skip"));
	xhashmapPut(ictx.opts, xstrdup("partition.bootloader:len"),
			xasprintf("%llu", to_mib(esp_size)));
	xhashmapPut(ictx.opts, xstrdup("partition.bootloader:index"),
			xasprintf("%u", esp_index));
	xhashmapPut(ictx.opts, xstrdup("partition.bootloader:device"),
			get_device_node(gpt, esp_index));
	create_android_partitions(esp_size, partlist, gpt, true);
	name = gpt_name("bootloader");
	esp = gpt_entry_get(esp_index, gpt);
	if (!esp)
		die("couldn't reference ESP");
	if (gpt_entry_set_name(esp, name))
		die("failure setting partition name to '%s'", name);
	free(name);
	return gpt;
}


struct gpt *execute_wipe_disk(char *partlist, char *device)
{
	uint64_t bootloader_size;
	struct gpt *gpt;

	gpt = gpt_init(device);
	if (!gpt)
		die("gpt_init");
	if (gpt_new(gpt))
		die("coudln't create new GPT");

	bootloader_size = (xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.bootloader:len")) << 20) * 2;
	create_android_partitions(bootloader_size, partlist, gpt, false);
	return gpt;
}


static bool getguid_cb(char *entry, int list_index _unused, void *context)
{
	struct gpt *gpt = context;
	int index = xatol(hashmapGetPrintf(ictx.opts, NULL,
			"partition.%s:index", entry));
	struct gpt_entry *e = gpt_entry_get(index, gpt);
	char *guid = gpt_guid_to_string(&(e->part_guid));
	if (!guid)
		die("gpt_guid_to_string");
	xhashmapPut(ictx.opts, xasprintf("partition.%s:guid", entry), guid);
	return true;
}


static void partitioner_execute(void)
{
	char *disk, *device, *partlist, *buf;
	bool dualboot;
	struct gpt *gpt;

	partlist = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);
	disk = hashmapGetPrintf(ictx.opts, NULL,
				BASE_INSTALL_DISK);
	device = xasprintf("/dev/block/%s", disk);
	dualboot = xatol(hashmapGetPrintf(ictx.opts, "0",
				"base:dualboot"));
	set_install_id();
	if (dualboot) {
		gpt = execute_dual_boot(disk, partlist, device);
	} else {
		gpt = execute_wipe_disk(partlist, device);
	}

	buf = gpt_dump_header(gpt);
	if (buf) {
		pr_debug("%s", buf);
		free(buf);
	}
	buf = gpt_dump_pentries(gpt);
	if (buf) {
		char *buf2, *line;
		for (buf2 = buf; ; buf2 = NULL) {
			line = strtok(buf2, "\n");
			if (!line)
				break;
			pr_debug("%s", line);
		}
		free(buf);
	}

	/* Set all the partition.XX:guid entries */
	string_list_iterate(partlist, getguid_cb, gpt);
	if (gpt_write(gpt))
		die("Couldn't write GPT");
	gpt_close(gpt);
	gpt_sync_ptable(device);
	free(device);
	pr_debug("Partitioner execution phase complete");
}


static struct iago_plugin plugin = {
	.prepare = partitioner_prepare,
	.cli_session = partitioner_cli,
	.execute = partitioner_execute
};


struct iago_plugin *partitioner_init(void)
{
	return &plugin;
}
