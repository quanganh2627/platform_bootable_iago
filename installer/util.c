/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/fs.h>

#include <zlib.h>
#include <cutils/android_reboot.h>
#include <cutils/properties.h>
#include <make_ext4fs.h>

#include <iago.h>
#include <iago_util.h>


void __die(const char *fmt, ...)
{
	va_list ap;
	char buf[4096];
	int len;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	len = strlen(buf);
	while (buf[--len] == '\n')
		buf[len] = 0;

	printf("%s\n", buf);
	property_set("iago.error", buf);
	LOG_ALWAYS_FATAL("%s", buf);
	exit(EXIT_FAILURE); /* shouldn't get here */
}

void *xmalloc(size_t size)
{
	void *ret = malloc(size);
	if (!ret)
		die_errno("malloc allocation size: %d", size);
	return ret;
}


char *xstrdup(const char *s)
{
	char *ret = strdup(s);
	if (!ret)
		die_errno("strdup");
	return ret;
}


char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	int ret;
	char *out;

	va_start(ap, fmt);
	ret = vasprintf(&out, fmt, ap);
	va_end(ap);

	if (!ret)
		die_errno("asprintf");
	return out;
}


static ssize_t robust_read(int fd, void *buf, size_t count, bool short_ok)
{
	unsigned char *pos = buf;
	ssize_t ret;
	ssize_t total = 0;
	do {
		ret = read(fd, pos, count);
		if (ret < 0) {
			if (errno != EINTR)
				die_errno("read");
			else
				continue;
		}
		count -= ret;
		pos += ret;
		total += ret;
	} while (count && !short_ok);
	return total;
}


ssize_t xwrite(int fd, const void *buf, size_t count)
{
	const char *pos = buf;
	ssize_t total_written = 0;

	while (count) {
		ssize_t written = write(fd, pos, count);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			die_errno("write");
		}
		count -= written;
		pos += written;
		total_written += written;
	}
	return total_written;
}


#define CHUNK	1024 * 1024
static void __dd(const char *src, const char *dest, bool copy_ok)
{
	char buf[CHUNK];
	ssize_t to_write;
	int ifd, ofd;
	size_t total_written = 0;

	ifd = xopen(src, O_RDONLY);
	ofd = xopen(dest, copy_ok ? (O_WRONLY | O_CREAT) : O_WRONLY);

	while (1) {
		to_write = robust_read(ifd, buf, CHUNK, true);
		if (!to_write)
			break;
		total_written += xwrite(ofd, buf, to_write);
	}
	xclose(ifd);
	xclose(ofd);

	pr_debug("Wrote %zu bytes from %s to %s", total_written,
			src, dest);
}


void dd(const char *src, const char *dest)
{
	__dd(src, dest, false);
}


void copy_file(const char *src, const char *dest)
{
	__dd(src, dest, true);
}


void mount_partition_device(const char *device, const char *type, char *mountpoint)
{
	int ret;

	ret = mkdir(mountpoint, 0777);
	if (ret && errno != EEXIST)
		die_errno("mkdir");

	pr_debug("Mounting %s (%s) --> %s\n", device,
			type, mountpoint);
	ret = mount(device, mountpoint, type, MS_SYNCHRONOUS, "");
	if (ret && errno != EBUSY)
		die_errno("mount");
}


uint64_t get_volume_size(const char *device)
{
	int fd;
	uint64_t sz;

	fd = xopen(device, O_RDONLY);

	if (ioctl(fd, BLKGETSIZE64, &sz) < 0)
		die_errno("BLKGETSIZE64");

	xclose(fd);
	return sz;
}


#define FSCK_MSDOS_BIN      "/system/bin/fsck_msdos"
void vfat_filesystem_checks(const char *device)
{
	int rv;
	int pass = 1;

	/* copied from /system/vold/Fat.cpp */
	pr_debug("Running fsck_msdos... This MAY take a while.");
	do {
		rv = execute_command(FSCK_MSDOS_BIN "-p -f %s", device);
		switch(rv) {
		case 0:
			pr_debug("Filesystem check completed OK");
			return;
		case 2:
			die("Filesystem check failed (not a FAT filesystem)");
		case 4:
			if (pass++ <= 3) {
				pr_debug("Filesystem modified - rechecking (pass %d)",
						pass);
				continue;
			}
			die("Failing check after too many rechecks");
		default:
			die("Filesystem check failed (unknown exit code %d)", rv);
		}
	} while (0);
}


void ext4_filesystem_checks(const char *device, size_t footer)
{
	int ret;
	uint64_t length;

	/* run fdisk to make sure the partition is OK */
	ret = execute_command("/system/bin/e2fsck -C 0 -fn %s", device);
	if (ret) {
		die("fsck of filesystem %s failed\n", device);
	}

	length = get_volume_size(device) - footer;
	if (execute_command("/system/bin/resize2fs -f -F %s %lluK",
				device, length >> 10)) {
		die("could not resize filesystem to %lluK",
				length >> 10);
	}

	/* Set mount count to 1 so that 1st mount on boot doesn't
	 * result in complaints */
	if (execute_command("/system/bin/tune2fs -C 1 %s",
				device)) {
		die("tune2fs failed\n");
	}
}

int execute_command(const char *fmt, ...)
{
	int ret = -1;
	va_list ap;
	char *cmd;

	va_start(ap, fmt);
	if (vasprintf(&cmd, fmt, ap) < 0) {
		pr_perror("vasprintf");
		return -1;
	}
	va_end(ap);

	pr_debug("Executing: '%s'\n", cmd);
	ret = system(cmd);

	if (ret < 0) {
		pr_error("Error while trying to execute '%s': %s",
			cmd, strerror(errno));
		goto out;
	}
	ret = WEXITSTATUS(ret);
	pr_debug("Done executing '%s' (retval=%d)\n", cmd, ret);
out:
	free(cmd);
	return ret;
}

int execute_command_data(void *data, unsigned sz, const char *fmt, ...)
{
	int ret = -1;
	va_list ap;
	char *cmd;
	FILE *fp;
	size_t bytes_written;

	va_start(ap, fmt);
	if (vasprintf(&cmd, fmt, ap) < 0) {
		pr_perror("vasprintf");
		return -1;
	}
	va_end(ap);

	pr_debug("Executing: '%s'\n", cmd);
	fp = popen(cmd, "w");
	free(cmd);
	if (!fp) {
		pr_perror("popen");
		return -1;
	}

	bytes_written = fwrite(data, 1, sz, fp);
	if (bytes_written != sz) {
		pr_perror("fwrite");
		pclose(fp);
		return -1;
	}

	ret = pclose(fp);
	if (ret < 0) {
		pr_perror("pclose");
		return -1;
	}
	ret = WEXITSTATUS(ret);
	pr_debug("Execution complete, retval=%d\n", ret);

	return ret;
}


int is_valid_blkdev(const char *node)
{
	struct stat statbuf;
	if (stat(node, &statbuf)) {
		pr_perror("stat");
		return 0;
	}
	if (!S_ISBLK(statbuf.st_mode)) {
		pr_error("%s is not a block device\n", node);
		return 0;
	}
	return 1;
}


bool str_equals(void *keyA, void *keyB)
{
	char *s1 = (char *)keyA;
	char *s2 = (char *)keyB;
	return !strcmp(s1, s2);
}

int str_hash(void *key)
{
	char *s = (char *)key;
	return hashmapHash(s, strlen(s));
}

static bool hashmap_dump_cb(void *key, void *value, void *context _unused)
{
	pr_debug("[%s] = '%s'\n", (char *)key, (char *)value);
	return true;
}

void hashmap_dump(Hashmap *h)
{
	/* FIXME this would be much easier to read if the keys were
	 * sorted first */
	hashmapForEach(h, hashmap_dump_cb, NULL);
}

static bool hashmap_destroy_cb(void *key, void *value, void *context _unused) {
	free(key);
	free(value);
	return true;
}


void hashmap_destroy(Hashmap *h)
{
	hashmapForEach(h, hashmap_destroy_cb, NULL);
	hashmapFree(h);
}

void hashmap_add_dictionary(Hashmap *h, dictionary *d)
{
	int i;

	for (i=0; i < d->size; i++) {
		char *k, *v;
		k = d->key[i];
		v = d->val[i];
		if (!k || !v)
			continue;

		xhashmapPut(h, xstrdup(k), xstrdup(v));
	}
}

void string_list_iterate(char *stringlist, bool (*cb)(char *entry,
			int index, void *context), void *context)
{
	char *saveptr, *entry, *str;
	int idx = 0;
	char *list;

	if (!stringlist)
		return;

	list = xstrdup(stringlist);
	for (str = list; ; str = NULL) {
		entry = strtok_r(str, " \t", &saveptr);
		if (!entry)
			break;
		if (!cb(entry, idx++, context))
			break;
	}
	free(list);
}

char *hashmapGetPrintf(Hashmap *h, void *dfl, const char *fmt, ...)
{
	char *key;
	void *value;
	va_list ap;

	va_start(ap, fmt);
	if (vasprintf(&key, fmt, ap) < 0)
		die_errno("vasprintf");

	va_end(ap);

	value = hashmapGet(h, key) ? : dfl;
	if (!value)
		die("failed to find required option entry '%s'", key);

	free(key);
	return (char *)value;
}

char *xhashmapPut(Hashmap *h, void *key, void *value)
{
	void *ret;

	errno = 0;
	ret = hashmapPut(h, key, value);
	if (ret)
		/* Entry already existing; free key so it isn't leaked */
		free(key);
	else if (errno)
		die_errno("hashmapPut");

	return ret;
}

void write_opts(Hashmap *h, const char *filename) {
	(void)filename;
	(void)h;
	die("write_opts stub!");
}

void put_string(int fd, const char *fmt, ...)
{
	char *buf, *buf_ptr;
	va_list ap;
	ssize_t to_write;

	va_start(ap, fmt);
	if (vasprintf(&buf, fmt, ap) < 0)
		die_errno("vasprintf");

	va_end(ap);

	buf_ptr = buf;
	to_write = strlen(buf);
	while (to_write) {
		ssize_t written = write(fd, buf_ptr, to_write);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			die_errno("write");
		}
		to_write -= written;
		buf_ptr += written;
	}
	free(buf);
}

void string_list_append(char **list, char *entry)
{
	char *newlist;

	if (!(*list) || strlen(*list) == 0) {
		free(*list);
		*list = xasprintf("%s", entry);
	} else {
		newlist = xasprintf("%s %s", *list, entry);
		free(*list);
		*list = newlist;
	}
}


ssize_t xread(int fd, void *buf, size_t count)
{
	return robust_read(fd, buf, count, false);
}


static char *__read_sysfs(const char *fmt, va_list ap)
{
	int fd;
	char buf[4096];
	char *filename;
	ssize_t bytes_read;

	if (vasprintf(&filename, fmt, ap) < 0) {
		die_errno("vasprintf");
	}

	fd = xopen(filename, O_RDONLY);

	free(filename);

	bytes_read = robust_read(fd, buf, sizeof(buf) - 1, true);
	buf[bytes_read] = '\0';
	while (bytes_read && buf[--bytes_read] == '\n')
		buf[bytes_read] = '\0';
	return xstrdup(buf);
}


char *read_sysfs(const char *fmt, ...)
{
	va_list ap;
	char *ret;

	va_start(ap, fmt);
	ret = __read_sysfs(fmt, ap);
	va_end(ap);
	return ret;
}


int64_t read_sysfs_int(const char *fmt, ...)
{
	int64_t val;
	char *vs;
	va_list ap;

	va_start(ap, fmt);
	vs = __read_sysfs(fmt, ap);
	va_end(ap);

	val = xatoll(vs);
	free(vs);
	return val;
}

void ui_printf(const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	fputs("iago: ", stdout);
	va_start(ap, fmt);
	vsnprintf(buf, 256, fmt, ap);
	va_end(ap);

	fputs(buf, stdout);
	if (buf[strlen(buf) - 1] != '\n')
		fputs("\n", stdout);
}

off_t xlseek(int fd, off_t offset, int whence)
{
	off_t ret;
	ret = lseek(fd, offset, whence);
	if (ret < 0)
		die_errno("lseek");
	return ret;
}


long int xatol(const char *nptr)
{
	char *endptr;
	long int val;

	if (!nptr)
		die("null value");

	errno = 0;
	val = strtol(nptr, &endptr, 10);
	if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
			|| (errno != 0 && val == 0))
		die_errno("strtol '%s'", nptr);
	if (endptr == nptr)
		die("no digits found in '%s'", nptr);
	return val;
}


long long int xatoll(const char *nptr)
{
	char *endptr;
	long long int val;

	if (!nptr)
		die("null value");

	errno = 0;
	val = strtoll(nptr, &endptr, 10);
	if ((errno == ERANGE && (val == LLONG_MAX || val == LLONG_MIN))
			|| (errno != 0 && val == 0))
		die_errno("strtoll '%s'", nptr);
	if (endptr == nptr)
		die("no digits found in '%s'", nptr);
	return val;
}


int xopen(const char *pathname, int flags)
{
	int rv;

	rv = open(pathname, flags, S_IRUSR | S_IWUSR);
	if (rv < 0)
		die_errno("open '%s'", pathname);
	return rv;
}

void xclose(int fd)
{
	if (close(fd) < 0)
		die_errno("close");
}

int make_ext4fs_nowipe(const char *filename, int64_t len,
                char *mountpoint, struct selabel_handle *sehnd)
{
	int fd;
	int status;

	reset_ext4fs_info();
	info.len = len;

	fd = xopen(filename, O_WRONLY);
	status = make_ext4fs_internal(fd, NULL, mountpoint, NULL, 0, 0, 0, 0, 0, sehnd);
	xclose(fd);

	return status;
}

