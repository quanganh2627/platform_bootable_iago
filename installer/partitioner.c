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

static struct gpt_entry *get_entry(unsigned int entry_index,
		struct gpt_header *hdr, unsigned char *entries)
{
	struct gpt_entry *e;
	if (entry_index >= letoh32(hdr->num_pentries))
		die("invalid GPT entry %d (max %d)", entry_index,
				letoh32(hdr->num_pentries));

	e = (struct gpt_entry *)(entries + (letoh32(hdr->pentry_size) *
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


static void dump_pentries(struct gpt_header *hdr, unsigned char *entries)
{
	uint32_t i;
	pr_debug("----------- GPT ENTRIES -------------\n");
	for (i = 0; i < letoh32(hdr->num_pentries); i++) {
		struct gpt_entry *e = get_entry(i, hdr, entries);
		if (e->first_lba)
			dump_pentry(e);
	}
	pr_debug("-------------------------------------\n");
}


static void dump_header(struct gpt_header *hdr)
{
	char *disk_guid = guid_to_string(&(hdr->disk_guid));
	pr_debug("------------ GPT HEADER -------------\n");
	pr_debug("             rev: 0x%08X\n", letoh32(hdr->revision));
	pr_debug("        hdr_size: %u\n", letoh32(hdr->header_size));
	pr_debug("           crc32: 0x%08X\n", letoh32(hdr->crc32));
	pr_debug("     current_lba: %llu\n", letoh64(hdr->current_lba));
	pr_debug("      backup_lba: %llu\n", letoh64(hdr->backup_lba));
	pr_debug("first_usable_lba: %llu\n", letoh64(hdr->first_usable_lba));
	pr_debug(" last_usable_lba: %llu\n", letoh64(hdr->last_usable_lba));
	pr_debug("       disk_guid: %s\n", disk_guid);
	pr_debug("pentry_start_lba: %llu\n", letoh64(hdr->pentry_start_lba));
	pr_debug("    num_pentries: %u\n", letoh32(hdr->num_pentries));
	pr_debug("     pentry_size: %u\n", letoh32(hdr->pentry_size));
	pr_debug("    pentry_crc32: 0x%08X\n", letoh32(hdr->pentry_crc32));
	pr_debug("-------------------------------------\n");
}


static int read_gpt(const char *device, struct gpt_header *hdr,
		unsigned char **entries_ptr)
{
	int fd;
	size_t entries_size;
	uint64_t lba_size;
	unsigned char *entries;
	uint8_t type;

	lba_size = xatoll(hashmapGetPrintf(ictx.opts, NULL, "disk.%s:lba_size",
				strrchr(device, '/')+1));
	pr_debug("LBA size is %llu", lba_size);

	fd = xopen(device, O_RDONLY);

	xlseek(fd, 0x1be + 0x4, SEEK_SET);
	xread(fd, &type, sizeof(type));
	if (type != 0xee) {
		/* First partition entry in the MBR isn't the protective
		 * entry. Let's get out of here */
		pr_debug("Disk %s doesn't seem to have a protective MBR",
				device);
		return -1;
	}

	xlseek(fd, 1 * lba_size, SEEK_SET);
	xread(fd, hdr, sizeof(*hdr));
	if (strncmp("EFI PART", hdr->sig, 8)) {
		/* Not a valid GPT. Technically we should also verify
		 * the checksums and read the backup GPT, but this is
		 * good enough for now */
		pr_debug("GPT header sig invalid\n");
		return -1;
	}
	entries_size = letoh32(hdr->num_pentries) * letoh32(hdr->pentry_size);
	entries = xmalloc(entries_size);
	xlseek(fd, letoh32(hdr->pentry_start_lba) * lba_size, SEEK_SET);
	xread(fd, entries, entries_size);
	xclose(fd);

	*entries_ptr = entries;

	return 0;
}


static void partitioner_prepare(void)
{
	/* Scan all the existing available disks and populate opts with their info */
	DIR *dir = NULL;
	char *disks = xstrdup("");
	char media[PROPERTY_VALUE_MAX];
	property_get("ro.iago.media", media, "");

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
		if (!strcmp(dp->d_name, media))
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
		string_list_append(&disks, dp->d_name);
	}
	closedir(dir);
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

static void partitioner_cli(void)
{
	list_declare(disk_list);

	string_list_iterate(hashmapGetPrintf(ictx.opts, NULL, BASE_DISK_LIST),
			disk_list_cb, &disk_list);

	xhashmapPut(ictx.opts, xstrdup(BASE_INSTALL_DEV),
				xasprintf("/dev/block/%s",
				ui_option_get("Choose disk to install Android:",
					&disk_list)));
	option_list_free(&disk_list);
}

struct mkpart_ctx {
	/* Disk device node we're working with */
	const char *device;

	/* numerical partition index in the GPT; updated with each iteration */
	int ptn_index;

	/* mb offset of the next partition to create */
	uint64_t next_mb;

	/* Size of non-growable partitions, plus one. Assuming /data
	 * expands to fit the rest of the space. If this is less than
	 * disk_mb we're in trouble */
	uint64_t ptn_mb;

	/* Total available space on the disk */
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
			mc->device, entry, mc->next_mb,
			mc->next_mb + part_mb)) {
		die("Parted failure!\n");
	}

	if (strlen(entry) > 27)
		die("Partition '%s' name too long!", entry);

	if (execute_command(PARTED " %s name %d %016llX%s",
			mc->device, mc->ptn_index, install_id, entry)) {
		die("Parted failure!\n");
	}
	mc->next_mb += part_mb;
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

struct getguid_ctx {
	struct gpt_header *hdr;
	unsigned char *entries;
};

static bool getguid_cb(char *entry, int index, void *context)
{
	struct getguid_ctx *gctx = context;
	struct gpt_entry *e = get_entry(index, gctx->hdr, gctx->entries);
	char *guid = guid_to_string(&(e->part_guid));
	xhashmapPut(ictx.opts, xasprintf("partition.%s:guid", entry), guid);
	return true;
}


static void partitioner_execute(void)
{
	char *device, *disk, *partlist;
	struct mkpart_ctx mc;
	struct getguid_ctx gctx;
	uint64_t lba_size;
	int fd;
	char *install_id_str;
	struct gpt_header hdr;
	unsigned char *entries = NULL;

	fd = xopen("/dev/urandom", O_RDONLY);
	xread(fd, &install_id, sizeof(install_id));
	xclose(fd);
	install_id_str = xasprintf("%016llX", install_id);
	if (property_set("ro.boot.install_id", install_id_str))
		die("Unable to set ro.boot.install_id");

	device = (char *)hashmapGetPrintf(ictx.opts, NULL,
				BASE_INSTALL_DEV);
	disk = strrchr(device, '/') + 1;

	pr_debug("Partitioning %s", device);
	if (execute_command(PARTED " %s mklabel gpt", device)) {
		pr_error("Parted failure!\n");
		die();
	}

	read_gpt(device, &hdr, &entries);
	dump_header(&hdr);

	mc.device = device;
	mc.next_mb = 1;
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

	free(entries);
	read_gpt(device, &hdr, &entries);
	dump_pentries(&hdr, entries);
	gctx.hdr = &hdr;
	gctx.entries = entries;
	string_list_iterate(partlist, getguid_cb, &gctx);

	/* Add kernel command line items */
	xhashmapPut(ictx.cmdline, xstrdup("androidboot.install_id"),
			install_id_str);
	free(entries);
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
