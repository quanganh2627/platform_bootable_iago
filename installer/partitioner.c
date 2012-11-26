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
#define LOG_TAG "iago partitioner"

#include <ctype.h>
#include <endian.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fs.h>

#include <cutils/properties.h>

#include <iago.h>
#include <iago_util.h>

#include "iago_private.h"

#define PARTED "/installmedia/images/parted --machine --script --align optimal"

struct guid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8]; /* Does not get byte-swapped */
} __attribute__((__packed__));

/* All fields must have little-endian byte ordering! */
struct gpt_header {
	char sig[8];
	uint32_t revision;
	uint32_t header_size;
	uint32_t crc32;
	uint32_t reserved_zero;
	uint64_t current_lba;
	uint64_t backup_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	struct guid disk_guid;
	uint64_t pentry_start_lba;
	uint32_t num_pentries;
	uint32_t pentry_size;
	uint32_t pentry_crc32;
} __attribute__((__packed__));

struct gpt_entry {
	struct guid type_guid;
	struct guid part_guid;
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t flags;
	uint16_t name[36]; /* UTF-16LE */
} __attribute__((__packed__));

static struct gpt_header hdr;
static unsigned char *entries;
static uint64_t install_id;

static void sync_ptable(const char *device)
{
	int fd;
	pr_debug("synchonizing partition table for %s", device);
	sync();
	fd = xopen(device, O_RDWR);
	if (ioctl(fd, BLKRRPART, NULL))
		die_errno("BLKRRPART");
	xclose(fd);
}

static inline int getbyte(uint64_t val, int bytenum)
{
	return (int)((val >> (bytenum * 8)) & 0xFF);
}

static char *guid_to_string(struct guid *g)
{
	/* GUIDS are always printed with little-endian encoding */
	return xasprintf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			getbyte(g->data1, 0), getbyte(g->data1, 1),
			getbyte(g->data1, 2), getbyte(g->data1, 3),
			getbyte(g->data2, 0), getbyte(g->data2, 1),
			getbyte(g->data3, 0), getbyte(g->data3, 1),
			g->data4[0], g->data4[1], g->data4[2], g->data4[3],
			g->data4[4], g->data4[5], g->data4[6], g->data4[7]);
}

static struct gpt_entry *get_entry(unsigned int entry_index)
{
	struct gpt_entry *e;
	if (entry_index >= letoh32(hdr.num_pentries))
		die("invalid GPT entry %d (max %d)", entry_index,
				letoh32(hdr.num_pentries));

	e = (struct gpt_entry *)(entries + (letoh32(hdr.pentry_size) *
				entry_index));
	return e;
}


static void dump_pentry(struct gpt_entry *ent)
{
	char namebuf[36];
	size_t i;
	char *partguidstr, *typeguidstr;

	/* XXX This is NOT how to do utf16le to char * conversion! */
	for(i = 0; i < sizeof(namebuf); i++)
		namebuf[i] = (int8_t)letoh16(ent->name[i]);

	typeguidstr = guid_to_string(&ent->type_guid);
	partguidstr = guid_to_string(&ent->part_guid);
	pr_debug("%s %s %12llu %12llu 0x%016llX %s\n", typeguidstr, partguidstr,
			ent->first_lba, ent->last_lba, ent->flags, namebuf);
	free(typeguidstr);
	free(partguidstr);
}


static void dump_pentries(void)
{
	uint32_t i;
	pr_debug("----------- GPT ENTRIES -------------\n");
	for (i = 0; i < letoh32(hdr.num_pentries); i++) {
		struct gpt_entry *e = get_entry(i);
		if (e->first_lba)
			dump_pentry(get_entry(i));
	}
	pr_debug("-------------------------------------\n");
}


static void dump_header(void)
{
	char *disk_guid = guid_to_string(&(hdr.disk_guid));
	pr_debug("------------ GPT HEADER -------------\n");
	pr_debug("             rev: 0x%08X\n", letoh32(hdr.revision));
	pr_debug("        hdr_size: %u\n", letoh32(hdr.header_size));
	pr_debug("           crc32: 0x%08X\n", letoh32(hdr.crc32));
	pr_debug("     current_lba: %llu\n", letoh64(hdr.current_lba));
	pr_debug("      backup_lba: %llu\n", letoh64(hdr.backup_lba));
	pr_debug("first_usable_lba: %llu\n", letoh64(hdr.first_usable_lba));
	pr_debug(" last_usable_lba: %llu\n", letoh64(hdr.last_usable_lba));
	pr_debug("       disk_guid: %s\n", disk_guid);
	pr_debug("pentry_start_lba: %llu\n", letoh64(hdr.pentry_start_lba));
	pr_debug("    num_pentries: %u\n", letoh32(hdr.num_pentries));
	pr_debug("     pentry_size: %u\n", letoh32(hdr.pentry_size));
	pr_debug("    pentry_crc32: 0x%08X\n", letoh32(hdr.pentry_crc32));
	pr_debug("-------------------------------------\n");
}


static void read_gpt(const char *device)
{
	int fd;
	size_t entries_size;
	uint64_t lba_size;

	lba_size = xatoll(hashmapGetPrintf(ictx.opts, NULL, "disk.%s:lba_size",
				strrchr(device, '/')+1));
	pr_debug("LBA size is %llu", lba_size);

	fd = xopen(device, O_RDONLY);

	xlseek(fd, 1 * lba_size, SEEK_SET);
	xread(fd, &hdr, sizeof(hdr));

	entries_size = letoh32(hdr.num_pentries) * letoh32(hdr.pentry_size);
	entries = xmalloc(entries_size);
	xlseek(fd, letoh32(hdr.pentry_start_lba) * lba_size, SEEK_SET);
	xread(fd, entries, entries_size);
	xclose(fd);
}


static void partitioner_prepare(void)
{
	/* Scan all the existing available disks and populate opts with their info */
	DIR *dir = NULL;

	dir = opendir("/sys/block");
	if (!dir)
		die();

	while (1) {
		struct dirent *dp = readdir(dir);
		uint64_t sectors, lba_size;
		if (!dp)
			break;
		if (!strncmp(dp->d_name, ".", 1) ||
				!strncmp(dp->d_name, "ram", 3) ||
				!strncmp(dp->d_name, "loop", 4))
			continue;

		sectors = read_sysfs_int("/sys/block/%s/size", dp->d_name);
		lba_size = read_sysfs_int("/sys/block/%s/queue/logical_block_size",
				dp->d_name);

		xhashmapPut(ictx.opts, xasprintf("disk.%s:sectors", dp->d_name),
				xasprintf("%llu", sectors));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:lba_size", dp->d_name),
				xasprintf("%llu", lba_size));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:size", dp->d_name),
				xasprintf("%llu", (lba_size * sectors) >> 20));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:model", dp->d_name),
				read_sysfs("/sys/block/%s/device/model", dp->d_name));
	}
	closedir(dir);
}


static void partitioner_cli(void)
{
	// set base:install_device
	xhashmapPut(ictx.opts, xstrdup(BASE_INSTALL_DEV), xstrdup("/dev/block/sda"));
}

struct mkpart_ctx {
	const char *device;
	int ptn_index;
	uint64_t last_mb;
	uint64_t total_mb;
	uint64_t ptn_mb;
	uint64_t disk_mb;
};


static bool flags_cb(char *flag, int _unused index, void *context)
{
	struct mkpart_ctx *mc = context;
	bool enable;

	if (flag[0] == '!') {
		flag++;
		enable = false;
	} else {
		enable = true;
	}

	if (execute_command(PARTED " %s set %d %s %s", mc->device,
				mc->ptn_index, flag, enable ? "on" : "off")) {
		die("Unable to set partition %d flag %s", mc->ptn_index, flag);
	}
	return true;
}


static bool mkpart_cb(char *entry, int index _unused, void *context)
{
	struct mkpart_ctx *mc = context;
	char *flags;

	int64_t part_mb = xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.%s:len", entry));
	if (part_mb < 0)
		part_mb = mc->disk_mb - mc->ptn_mb;
	mc->ptn_index++;
	if (execute_command(PARTED " %s mkpart %s %lldMiB %lldMiB",
			mc->device, entry, mc->last_mb,
			mc->last_mb + part_mb)) {
		die("Parted failure!\n");
	}

	if (strlen(entry) > 27)
		die("Partition '%s' name too long!", entry);

	if (execute_command(PARTED " %s name %d %016llX%s",
			mc->device, mc->ptn_index, install_id, entry)) {
		die("Parted failure!\n");
	}
	mc->last_mb += part_mb;
	flags = hashmapGetPrintf(ictx.opts, "", "partition.%s:flags", entry);
	string_list_iterate(flags, flags_cb, mc);
	xhashmapPut(ictx.opts, xasprintf("partition.%s:index", entry),
			xasprintf("%d", mc->ptn_index));
	xhashmapPut(ictx.opts, xasprintf("partition.%s:device", entry),
			xasprintf("/dev/block/by-name/%s", entry));
	xhashmapPut(ictx.opts, xasprintf("partition.%s:index", entry),
			xasprintf("%d", mc->ptn_index));
	return true;
}


static bool sumsizes_cb(char *entry, int index _unused, void *context)
{
	uint64_t *ptn_mb = context;
	int64_t len = xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.%s:len", entry));
	if (len >= 0)
		*ptn_mb += len;
	return true;
}


static bool getguid_cb(char *entry, int index, void *context _unused)
{
	char *guid = guid_to_string(&(get_entry(index)->part_guid));
	xhashmapPut(ictx.opts, xasprintf("partition.%s:guid", entry), guid);
	return true;
}


static void partitioner_execute(void)
{
	char *device, *disk, *partlist;
	struct mkpart_ctx mc;
	uint64_t lba_size;
	int fd;
	char *install_id_str;

	fd = xopen("/dev/urandom", O_RDONLY);
	xread(fd, &install_id, sizeof(install_id));
	xclose(fd);
	install_id_str = xasprintf("%016llX", install_id);
	property_set("ro.boot.install_id", install_id_str);

	device = (char *)hashmapGetPrintf(ictx.opts, NULL,
				BASE_INSTALL_DEV);
	disk = strrchr(device, '/') + 1;

	pr_debug("Partitioning %s", device);
	if (execute_command(PARTED " %s mklabel gpt", device)) {
		pr_error("Parted failure!\n");
		die();
	}

	read_gpt(device);
	dump_header();

	mc.device = device;
	mc.last_mb = 1;
	mc.ptn_index = 0;
	mc.ptn_mb = 1;

	partlist = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);

	string_list_iterate(partlist, sumsizes_cb, &mc.ptn_mb);

	/* check sizes... */
	pr_debug("Total fixed partition sizes: %llu", mc.ptn_mb);
	lba_size = xatoll(hashmapGetPrintf(ictx.opts, NULL, "disk.%s:lba_size",
				disk));

	mc.disk_mb = ((letoh64(hdr.last_usable_lba) -
			letoh64(hdr.first_usable_lba)) * lba_size) >> 20;
	pr_debug("Disk size: %llu", mc.disk_mb);
	if (mc.disk_mb < mc.ptn_mb)
		die("disk %s is too small", disk);

	string_list_iterate(partlist, mkpart_cb, &mc);

	sync_ptable(device);

	read_gpt(device);
	dump_pentries();
	string_list_iterate(partlist, getguid_cb, NULL);

	/* Add kernel command line items */
	xhashmapPut(ictx.cmdline, xstrdup("androidboot.install_id"),
			install_id_str);

	pr_debug("partitioner/execute complete");
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
