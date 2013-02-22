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


#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iago.h>
#include <iago_util.h>
#include "iago_private.h"

#define MKDOSFS_BIN		"/system/bin/newfs_msdos"


static bool execute_cb(char *entry, int index _unused, void *context _unused)
{
	char *type, *src, *device, *prefix, *mode;
	ssize_t footer;
	struct stat sb;
	int count = 20;

	pr_info("Processing %s partition\n", entry);

	prefix = xasprintf("partition.%s", entry);
	type = hashmapGetPrintf(ictx.opts, NULL, "%s:type", prefix);
	device = hashmapGetPrintf(ictx.opts, NULL, "%s:device", prefix);
	mode = hashmapGetPrintf(ictx.opts, NULL, "%s:mode", prefix);

	while (stat(device, &sb)) {
		if (!count--)
			die("stat %s", device);
		pr_info("Waiting for %s...", device);
		sleep(1);
	}

	if (!strcmp(mode, "format")) {
		pr_info("Formatting %s (%s)", device, type);
		if (!strcmp(type, "ext4")) {
			footer = xatol(hashmapGetPrintf(ictx.opts, "0",
						"%s:footer", prefix));
			pr_debug("make_ext4fs(%s, %ld, %s)", device, 0 - footer, entry);
			if (make_ext4fs_nowipe(device, 0 - footer, entry, sehandle)) {
			        pr_error("make_ext4fs failed\n");
				die();
			}
		} else if (!strcmp(type, "vfat") || !strcmp(type, "esp")) {
			int rv;
			rv = execute_command(MKDOSFS_BIN " -L %s %s", entry,
					device);
			if (rv) {
				pr_error(MKDOSFS_BIN " failed: retval=%d\n",
						rv);
				die();
			}
		} else {
			pr_error("unsupported fs type '%s'\n", type);
			die();
		}
	} else if (!strcmp(mode, "image")) {
		src = xasprintf("/installmedia/images/%s",
				(char *)hashmapGetPrintf(ictx.opts, NULL,
					"%s:src", prefix));

		pr_info("Writing %s (%s) -> %s", src, type, device);
		dd(src, device);
		free(src);
		if (!strcmp(type, "ext4")) {
			footer = atoi(hashmapGetPrintf(ictx.opts, "0",
						"%s:footer", prefix));
			ext4_filesystem_checks(device, footer);
		} else if (!strcmp(type, "vfat")) {
			vfat_filesystem_checks(device);
		}
	} else if (!strcmp(mode, "zero")) {
		int fd;
		void *data;

		fd = xopen(device, O_WRONLY);
		data = xcalloc(1, 1024 * 1024);
		while (1) {
			ssize_t ret;

			ret = write(fd, data, 1024 * 1024);
			if (ret == 0 || (ret < 0 && errno == ENOSPC))
				break;
			if (ret < 0)
				die_errno("write");
		}
		free(data);
	} else if (!strcmp(mode, "skip")) {
		/* probably special handling later; do nothing */
	} else {
		pr_error("unsupported mode '%s'\n", mode);
		die();
	}

	return true;
}


static void imagewriter_execute(void)
{
	char *partitions;

	partitions = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);
	string_list_iterate(partitions, execute_cb, NULL);
}

static struct iago_plugin plugin = {
	.execute = imagewriter_execute
};

struct iago_plugin *imagewriter_init(void)
{
	return &plugin;
}
