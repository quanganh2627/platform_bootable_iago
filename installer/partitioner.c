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

#define NAME_MAGIC	"ANDROID!"

#define MIN_DATA_PART_SIZE	350 /* CDD section 7.6.1 */

#define getbyte(x, s)		(((x) >> (8 * (s))) & 0xFF)

/* Lets you define guids exactly as written out little endian */
#define GPT_GUID(a, b, c, d1, d2) \
	((struct guid) \
	 { htole32(a), htole16(b), htole16(c), \
	    { getbyte(d1, 1), getbyte(d1, 0), getbyte(d2, 5), getbyte(d2, 4), \
	       getbyte(d2, 3), getbyte(d2, 2), getbyte(d2, 1), getbyte(d2, 0) } } )

#define partition_for_each(gpt, i, e) \
	for ((i) = 1, (e) = get_entry((i), (gpt)); \
			i <= letoh32((gpt)->header.num_pentries); \
			e = get_entry(++(i), (gpt))) if (!(e)->first_lba) continue; else

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

struct gpt {
	struct gpt_header header;
	unsigned char *entries;
	int lba_size;
	char *device;
};

static struct guid efi_sys_ptn = GPT_GUID(0xC12A7328, 0xF81F, 0x11D2,
		0xBA4B,	0x00A0C93EC93BULL);

static struct guid ms_reserved_ptn = GPT_GUID(0xE3C9E316, 0x0B5C, 0x4DB8,
		0x817D, 0xF92DF00215AEULL);

static struct guid ms_data_ptn = GPT_GUID(0xEBD0A0A2, 0xB9E5, 0x4433,
		0x87C0, 0x68B6B72699C7ULL);

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


static char *lechar16_to_char8(uint16_t *str16)
{
	int i, len = 0;
	uint16_t *pos;
	char *ret;

	pos = str16;
	while (*(pos++))
		len++;

	ret = xmalloc(len + 1);
	/* XXX This is NOT how to do utf16le to char * conversion! */
	for (i = 0; i < len; i++) {
		uint16_t p = letoh16(str16[i]);
		if (p > 127)
			ret[i] = '?';
		else
			ret[i] = p;
	}
	ret[i] = 0;
	return ret;
}


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
	xhashmapPut(ictx.cmdline, xstrdup("androidboot.install_id"),
			install_id_str);
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


/* Indexes start at 1, to correspond with the disk minor number */
static struct gpt_entry *get_entry(uint32_t entry_index, struct gpt *gpt)
{
	struct gpt_entry *e;
	if (!entry_index || entry_index > letoh32(gpt->header.num_pentries))
		return NULL;

	e = (struct gpt_entry *)(gpt->entries + (letoh32(gpt->header.pentry_size) *
				--entry_index));
	return e;
}


/* Return the first index in the partition table that doesn't have an
 * actual partition entry. Used to predict the index of a new partition
 * created by parted */
static uint32_t get_unused_ptn_index(struct gpt *gpt)
{
	uint32_t i;

	for (i = 1; i <= letoh32(gpt->header.num_pentries); i++) {
		struct gpt_entry *e = get_entry(i, gpt);
		if (!e->first_lba)
			return i;
	}
	die("partition table is full");
}


static uint64_t get_entry_size(unsigned int entry_index, struct gpt *gpt)
{
	struct gpt_entry *e = get_entry(entry_index, gpt);
	if (!e)
		die("Invalid entry %u", entry_index);
	return (letoh64(e->last_lba) - letoh64(e->first_lba)) * gpt->lba_size;
}


static void dump_pentry(uint32_t index, struct gpt_entry *ent)
{
	char *partguidstr, *typeguidstr, *namebuf;

	namebuf = lechar16_to_char8(ent->name);
	typeguidstr = guid_to_string(&ent->type_guid);
	partguidstr = guid_to_string(&ent->part_guid);

	pr_debug("[%02d] %s %s %12llu %12llu 0x%016llX %s\n", index, typeguidstr,
			partguidstr, ent->first_lba, ent->last_lba, ent->flags,
			namebuf);
	free(namebuf);
	free(typeguidstr);
	free(partguidstr);
}


static void dump_pentries(struct gpt *gpt)
{
	uint32_t i;
	struct gpt_entry *e;

	pr_debug("----------- GPT ENTRIES -------------\n");
	partition_for_each(gpt, i, e) {
		dump_pentry(i, e);
	}
	pr_debug("-------------------------------------\n");
}


static void dump_header(struct gpt *gpt)
{
	char sig[9];
	struct gpt_header *hdr = &gpt->header;
	memcpy(sig, hdr->sig, 8);
	sig[8] = 0;
	char *disk_guid = guid_to_string(&(hdr->disk_guid));
	pr_debug("------------ GPT HEADER -------------\n");
	pr_debug("             sig: %s\n", sig);
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


static char *get_device_node(unsigned int gpt_index, struct gpt *gpt)
{
	if (isdigit(gpt->device[strlen(gpt->device) - 2]))
		return xasprintf("%sp%d", gpt->device, gpt_index);
	else
		return xasprintf("%s%d", gpt->device, gpt_index);
}


static void close_gpt(struct gpt *gpt)
{
	free(gpt->device);
	free(gpt->entries);
}


/* Read the GPT from the specified device node, filling in the fields
 * in the given struct gpt. Must eventually call close_gpt() on it. */
static int read_gpt(const char *device, struct gpt *gpt)
{
	int fd;
	size_t entries_size;
	uint64_t lba_size;
	uint8_t type;

	lba_size = xatoll(hashmapGetPrintf(ictx.opts, NULL, "disk.%s:lba_size",
				strrchr(device, '/')+1));

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
	xread(fd, &gpt->header, sizeof(struct gpt_header));
	if (strncmp("EFI PART", gpt->header.sig, 8)) {
		/* Not a valid GPT. Technically we should also verify
		 * the checksums and read the backup GPT, but this is
		 * good enough for now */
		pr_debug("GPT header sig invalid\n");
		return -1;
	}
	entries_size = letoh32(gpt->header.num_pentries) *
			letoh32(gpt->header.pentry_size);
	gpt->entries = xmalloc(entries_size);
	xlseek(fd, letoh32(gpt->header.pentry_start_lba) * lba_size, SEEK_SET);
	xread(fd, gpt->entries, entries_size);
	xclose(fd);
	gpt->lba_size = lba_size;
	gpt->device = xstrdup(device);
	return 0;
}


static void xread_gpt(const char *device, struct gpt *gpt)
{
	if (read_gpt(device, gpt))
		die("Failed to read GPT");
}


/* Re-read the GPT. Usually need to do this after we add or delete partitions
 * in parted */
static void regen_gpt(struct gpt *gpt)
{
	char *device = gpt->device;
	free(gpt->entries);
	xread_gpt(device, gpt);
	free(device);
}


/* Examine the disk for an existing Android installation,
 * returning the number of bytes it is taking up on the disk */
static uint64_t check_for_android(struct gpt *gpt)
{
	uint32_t i;
	struct gpt_entry *e;
	uint64_t size;

	size = 0;
	partition_for_each(gpt, i, e) {
		char *name = lechar16_to_char8(e->name);
		if (!strncmp(name, NAME_MAGIC, 8))
			size += get_entry_size(i, gpt);
	}
	return size;
}


/* Examine the disk for a partition that has the specified type
 * GUID. Return the index of the first one found */
static int check_for_ptn(struct gpt *gpt, struct guid *guid)
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


struct ptn_region {
	uint64_t start;
	uint64_t end;
	uint64_t size;
};


/* Comparison function to qsort() struct ptn_regions */
static int regioncmp(const void *a, const void *b)
{
	const struct ptn_region *pa = a;
	const struct ptn_region *pb = b;
	int64_t d = pa->start - pb->start;
	/* Avoid potential overflow by just returning d */
	if (d < 0)
		return -1;
	if (d > 0)
		return 1;
	return 0;
}


/* Examine an empty region and determine whether its size exceeds the
 * largest that we have already seen.
 * largest - data structure to update
 * base - beginning LBA of the empty region
 * start - starting LBA of the next partitioned area */
static void largest_update(struct ptn_region *largest, uint64_t base,
		uint64_t start)
{
	if (start > base) { /* if not, no gap at all */
		uint64_t size = start - base;
		if (size > largest->size) {
			largest->size = size;
			largest->start = base;
			largest->end = start - 1;
		}
	}
}


/* Return the start and end LBAs of the largest block of unpartitioned
 * space on the disk.
 *
 * returns 0 on success and start_lba, end_lba updated. both values inclusive.
 * returns -1 if there are no free regions at all
 */
static int find_contiguous_free_space(struct gpt *gpt, uint64_t *start_lba,
		uint64_t *end_lba)
{
	uint32_t ptn_count = 0;
	uint32_t i, j;
	uint64_t base;
	struct gpt_entry *e;
	struct ptn_region largest;
	struct ptn_region *regions;

	/* Create an array of all the (start, end) partition entries and sort
	 * it ascending by starting location */
	partition_for_each(gpt, i, e)
		ptn_count++;
	regions = xcalloc(ptn_count, sizeof(struct ptn_region));
	j = 0;
	partition_for_each(gpt, i, e) {
		regions[j].start = letoh64(e->first_lba);
		regions[j].end = letoh64(e->last_lba);
		j++;
	}
	qsort(regions, ptn_count, sizeof(struct ptn_region), regioncmp);

	/* Check for gaps, calculate their size, and update largest if
	 * bigger than already seen */
	base = letoh64(gpt->header.first_usable_lba);
	largest.size = 0;
	largest.end = 0;
	largest.start = 0;
	for (i = 0; i < ptn_count; i++) {
		largest_update(&largest, base, regions[i].start);
		base = regions[i].end + 1;
	}
	free(regions);

	/* Check space after last partition */
	largest_update(&largest, base, letoh64(gpt->header.last_usable_lba) + 1);

	if (largest.size == 0)
		return -1;
	*start_lba = largest.start;
	*end_lba = largest.end;
	pr_debug("find_contiguous_free_space: LBA %llu --> %llu (inclusive)",
			largest.start, largest.end);
	return 0;
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

	e = get_entry(index, gpt);
	if (memcmp(&e->type_guid, &ms_data_ptn, sizeof(struct guid)))
		return -EINVAL;

	device = get_device_node(index, gpt);
	ret = execute_command("ntfsresize --check %s", device);
	if (ret) {
		free(device);
		return -EROFS;
	}

	sz = sizeof(buf);
	ret = execute_command_output(buf, &sz,
			"ntfsresize --no-progress-bar --info %s", device);
	free(device);
	if (ret)
		return -EIO;
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
	uint64_t start_lba, end_lba;
	char *device;

	/* Assumes all checks in get_ntfs_min_size_mb have been done */
	e = get_entry(index, gpt);
	start_lba = letoh32(e->first_lba);
	end_lba = start_lba + to_unit_ceiling(new_size, gpt->lba_size);

	/* Resize the NTFS filesystem */
	device = get_device_node(index, gpt);
	ret = execute_command("ntfsresize --no-action --size %lld %s", new_size, device);
	if (ret)
		die("ntfs resize operation dry run failed");

	ret = execute_command("ntfsresize --force --size %lld %s", new_size, device);
	if (ret)
		die("ntfs resize operation failed. disk is likely corrupted!!");
	free(device);

	/* Now resize the underlying partition by deleting it and recreating */
	ret = execute_command(PARTED " %s rm %d", gpt->device, index);
	if (ret)
		die("Couldn't remove old NTFS partition entry");
	/* end_lba is inclusive in parted when specifying with sectors */
	ret = execute_command(PARTED " %s mkpart msdata ntfs %llds %llds",
			gpt->device, start_lba, end_lba - 1);
	if (ret)
		die("Couldn't create resized NTFS partition");
	regen_gpt(gpt);
}


static void delete_android(struct gpt *gpt)
{
	struct gpt_entry *e;
	uint32_t i;

	partition_for_each(gpt, i, e) {
		int ret;
		char *name;

		name = lechar16_to_char8(e->name);

		if (strncmp(name, NAME_MAGIC, 8))
                        continue;
		if (strlen(name) == 26 && !strcmp(name + 16, "bootloader"))
			continue;

		ret = execute_command(PARTED " %s rm %d", gpt->device, i);
		if (ret)
			die("Couldn't delete partition");
	}
	regen_gpt(gpt);
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
		uint64_t sectors, lba_size, android_size;
		struct gpt gpt;
		char *device;
		int ptn_index;

		if (!dp)
			break;
		if (!strncmp(dp->d_name, ".", 1) ||
				!strncmp(dp->d_name, "ram", 3) ||
				!strncmp(dp->d_name, "loop", 4))
			continue;
		if (!strcmp(dp->d_name, media))
			continue;

		device = xasprintf("/dev/block/%s", dp->d_name);
		sectors = read_sysfs_int("/sys/block/%s/size", dp->d_name);
		lba_size = read_sysfs_int("/sys/block/%s/queue/logical_block_size",
				dp->d_name);

		xhashmapPut(ictx.opts, xasprintf("disk.%s:sectors", dp->d_name),
				xasprintf("%llu", sectors));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:lba_size", dp->d_name),
				xasprintf("%llu", lba_size));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:size", dp->d_name),
				xasprintf("%llu", lba_size * sectors));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:model", dp->d_name),
				read_sysfs("/sys/block/%s/device/model", dp->d_name));
		xhashmapPut(ictx.opts, xasprintf("disk.%s:device", dp->d_name),
				device);
		string_list_append(&disks, dp->d_name);

		if (read_gpt(hashmapGetPrintf(ictx.opts, NULL, "disk.%s:device", dp->d_name),
					&gpt))
			continue;

		ptn_index = check_for_ptn(&gpt, &ms_reserved_ptn);
		if (ptn_index >= 0) {
			/* User data NTFS partition always right after
			 * the ms reserved partition */
			uint64_t ret, size;
			int ms_data_idx = ptn_index + 1;
			size = get_entry_size(ms_data_idx, &gpt);
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
			ret = get_ntfs_min_size(ms_data_idx, &gpt);
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
		} else
			pr_debug("%s: No Windows installation found\n", dp->d_name);

		ptn_index = check_for_ptn(&gpt, &efi_sys_ptn);
		if (ptn_index >= 0) {
			/* We found an EFI system partition and may
			 * re-use it */
			pr_debug("%s: Found ESP at partition index %d\n",
					dp->d_name, ptn_index);
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:esp_index", dp->d_name),
				xasprintf("%d", ptn_index));
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:esp_size", dp->d_name),
				xasprintf("%llu", get_entry_size(ptn_index, &gpt)));
		} else
			pr_debug("%s: no EFI System Partition found\n",
					dp->d_name);

		android_size = check_for_android(&gpt);
		if (android_size) {
			pr_info("Disk %s has an existing Android installation",
					dp->d_name);
			xhashmapPut(ictx.opts,
				xasprintf("disk.%s:android_size", dp->d_name),
				xasprintf("%llu", android_size));
		}

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
 * partition. Otherwise, its the combined size of both */
static uint64_t get_space_required(uint64_t esp_sizes)
{
	char *partlist;
	uint64_t total = 0;

	partlist = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);
	/* sum up all non-bootloader partitions */
	string_list_iterate(partlist, sumsizes_cb, &total);
	total += esp_sizes; /* for the copy */
	return total;
}


/* XXX FIXME TODO this is hacked-together crapola that doesn't do
 * enough checks and isn't flexible enough */
static void partitioner_cli(void)
{
	char *disk;
	uint64_t android_size, windows_size, windows_min_size;
	uint64_t total_free_size, required_size;
	uint64_t new_windows_size, esp_size;

	bool can_resize = false;

	list_declare(disk_list);

	string_list_iterate(hashmapGetPrintf(ictx.opts, NULL, BASE_DISK_LIST),
			disk_list_cb, &disk_list);

	disk = xstrdup(ui_option_get("Choose disk to install Android:",
					&disk_list));
	xhashmapPut(ictx.opts, xstrdup(BASE_INSTALL_DISK),
				xstrdup(disk));
	option_list_free(&disk_list);

	android_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:android_size", disk));
	windows_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:msdata_size", disk));
	windows_min_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:msdata_minsize", disk));
	esp_size = xatol(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:esp_size", disk));

	if (android_size)
		pr_info("Android installation detected. It will be deleted, freeing %llu MiB",
				to_mib(android_size));

	total_free_size = android_size;

	if (windows_size && esp_size) {
		pr_info("Windows data partition detected of size %llu MiB.",
				to_mib(windows_size));
		if (windows_min_size) {
			pr_info("Windows data partition can be shrunk to as low as %llu MiB.",
					to_mib(windows_min_size));
			can_resize = true;
		} else {
			pr_info("However, you need to reboot into Windows and run chkdsk on it, it cannot be resized right now.");
		}

		if (ui_ask("Do you want to preserve Windows and dual boot?", true)) {

			// XXX should be any unpartitioned space after windows data.
			// these calculations are somewhat complex and should be handled better
			// right now we're assuming android is immediately after windows data.
			// maybe present user with option to drop into interactive parted session.
			total_free_size = android_size;
			required_size = get_space_required(esp_size);
			pr_info("Total current free space is %llu MiB. We require at least %llu MiB.",
					to_mib(total_free_size), to_mib(required_size));

			if (can_resize && ui_ask("Do you want to resize Windows to make more space?", true)) {
				new_windows_size = ui_get_value("Enter new size in MiB for Windows",
						to_mib(windows_size),
						to_mib(windows_min_size),
						to_mib(windows_size));
				xhashmapPut(ictx.opts,
						xasprintf("disk.%s:windows_resize", disk),
						xasprintf("%llu", new_windows_size << 20));
			}
			xhashmapPut(ictx.opts, xstrdup("base:dualboot"), xstrdup("1"));
		}
	}
}


static char *gpt_name(const char *name)
{
	char *ret, *install_id;

	install_id = get_install_id();
	ret = xasprintf("%s%s", install_id, name);
	free(install_id);
	return ret;
}


struct flags_ctx {
	int index;
	const char *device;
};


static bool flags_cb(char *flag, int _unused index, void *context)
{
	struct flags_ctx *fc = context;
	bool enable;

	if (flag[0] == '!') {
		flag++;
		enable = false;
	} else {
		enable = true;
	}

	if (execute_command(PARTED " %s set %d %s %s", fc->device,
				fc->index, flag, enable ? "on" : "off")) {
		die("Unable to set partition %d flag %s", fc->index, flag);
	}
	return true;
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


static bool mkpart_cb(char *entry, int index _unused, void *context)
{
	struct mkpart_ctx *mc = context;
	struct flags_ctx fc;
	char *flags;
	char *name;
	int64_t part_mb;

	if (mc->skip_bootloader && !strcmp(entry, "bootloader"))
		return true;

	/* Android port of parted has some issue with specifying
	 * partition name along with mkpart. This prevents us from
	 * looking up the index by name, but we know that parted
	 * uses the next unused GPT entry in the entries array */
	mc->ptn_index = get_unused_ptn_index(mc->gpt);

	if (strlen(entry) > 27)
		die("Partition '%s' name too long!", entry);

	part_mb = xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.%s:len", entry));
	if (part_mb < 0)
		part_mb = mc->disk_mb - mc->ptn_mb;

	name = gpt_name(entry);
	if (execute_command(PARTED " %s mkpart %s ext4 %lldMiB %lldMiB",
			mc->gpt->device, name, mc->next_mb,
			mc->next_mb + part_mb))
		die("Parted failure creating new partition\n");

	/* wish I knew why we had to do this, whatever */
	if (execute_command(PARTED " %s name %d %s",
				mc->gpt->device, mc->ptn_index, name))
		die("Parted failure setting partition name to '%s'", name);

	free(name);
	regen_gpt(mc->gpt);
	mc->next_mb += part_mb;
	flags = hashmapGetPrintf(ictx.opts, "", "partition.%s:flags", entry);
	fc.index = mc->ptn_index;
	fc.device = mc->gpt->device;
	string_list_iterate(flags, flags_cb, &fc);
	xhashmapPut(ictx.opts, xasprintf("partition.%s:index", entry),
			xasprintf("%d", mc->ptn_index));
	xhashmapPut(ictx.opts, xasprintf("partition.%s:device", entry),
			xasprintf("/dev/block/by-name/%s", entry));
	return true;
}


static void create_android_partitions(uint64_t bootloader_size, char *partlist,
		struct gpt *gpt, bool skip_bootloader)
{
	uint64_t start_lba, end_lba, start_mb, end_mb;
	uint64_t space_needed_mb;
	uint64_t space_available_mb;
	struct mkpart_ctx mc;

	space_needed_mb = to_mib(get_space_required(bootloader_size));

	if (find_contiguous_free_space(gpt, &start_lba, &end_lba))
		die("Couldn't calculate unpartitioned disk space");

	start_mb = to_mib(start_lba * gpt->lba_size);
	end_mb = to_mib_floor((end_lba + 1) * gpt->lba_size);
	space_available_mb = end_mb - start_mb;
	if (space_available_mb < space_needed_mb)
		die("Insufficient disk space (have %llu MiB need %llu MiB)",
				space_available_mb,
				space_needed_mb);

	/* Always the same size */
	xhashmapPut(ictx.opts, xstrdup("partition.bootloader2:len"),
			xstrdup(hashmapGetPrintf(ictx.opts, NULL,
					"partition.bootloader:len")));

	mc.next_mb = start_mb;
	mc.disk_mb = space_available_mb;
	mc.skip_bootloader = skip_bootloader;
	mc.ptn_mb = space_needed_mb;
	mc.gpt = gpt;
	dump_header(gpt);

	pr_debug("offset=%llu space_available=%llu space_needed=%llu",
			mc.next_mb, mc.disk_mb, mc.ptn_mb);
	string_list_iterate(partlist, mkpart_cb, &mc);
}


static void execute_dual_boot(uint32_t win_index, char *disk,
		char *partlist, struct gpt *gpt)
{
	uint64_t esp_size, win_resize;
	uint32_t esp_index;
	char *name;

	win_resize = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:windows_resize", disk));
	esp_size = xatoll(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:esp_size", disk));
	esp_index = xatol(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:esp_index", disk));

	if (win_resize) {
		pr_info("Resizing Windows partition");
		resize_ntfs_partition(win_index, gpt, win_resize);
	}

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
			xstrdup("/dev/block/by-name/bootloader"));
	create_android_partitions(esp_size, partlist, gpt, true);
	name = gpt_name("bootloader");
	if (execute_command(PARTED " %s name %d %s",
				gpt->device, esp_index, name))
		die("Parted failure setting partition name to '%s'", name);
	free(name);
}


static void execute_wipe_disk(char *partlist, struct gpt *gpt)
{
	uint64_t bootloader_size;

	pr_debug("Creating new partition table for %s", gpt->device);
	if (execute_command(PARTED " %s mklabel gpt", gpt->device))
		die("Parted failure!\n");
	regen_gpt(gpt);
	bootloader_size = (xatoll(hashmapGetPrintf(ictx.opts, NULL,
				"partition.bootloader:len")) << 20) * 2;
	create_android_partitions(bootloader_size, partlist, gpt, false);
}


static bool getguid_cb(char *entry, int list_index _unused, void *context)
{
	struct gpt *gpt = context;
	int index = xatol(hashmapGetPrintf(ictx.opts, NULL,
			"partition.%s:index", entry));
	struct gpt_entry *e = get_entry(index, gpt);
	char *guid = guid_to_string(&(e->part_guid));
	xhashmapPut(ictx.opts, xasprintf("partition.%s:guid", entry), guid);
	return true;
}


static void partitioner_execute(void)
{
	char *disk, *device, *partlist;
	bool dualboot;
	struct gpt gpt;
	uint32_t win_index;

	partlist = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);
	disk = hashmapGetPrintf(ictx.opts, NULL,
				BASE_INSTALL_DISK);
	device = xasprintf("/dev/block/%s", disk);
	xread_gpt(device, &gpt);
	dualboot = xatol(hashmapGetPrintf(ictx.opts, "0",
				"base:dualboot"));
	/* Confirms that there is indeed a Windows data partition
	 * and hence dual boot is possible */
	win_index = xatol(hashmapGetPrintf(ictx.opts, "0",
				"disk.%s:msdata_index", disk));
	set_install_id();
	if (win_index && dualboot) {
		execute_dual_boot(win_index, disk, partlist, &gpt);
	} else {
		execute_wipe_disk(partlist, &gpt);
	}

	dump_header(&gpt);
	dump_pentries(&gpt);

	/* Set all the partition.XX:guid entries */
	string_list_iterate(partlist, getguid_cb, &gpt);
	close_gpt(&gpt);
	sync_ptable(device);
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
