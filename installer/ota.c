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


#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iago.h>
#include <iago_util.h>
#include "iago_private.h"


static void ota_execute(void)
{
	char *mountpoint;
	char *destfile;
	char *cmdline;
	char *srcfile;
	int fd;

	srcfile = hashmapGetPrintf(ictx.opts, "none", "base:ota");
	if (!strcmp("none", srcfile))
		return;

	pr_info("Mounting /cache...\n");
	mountpoint = mkdtemp(xstrdup("cache-XXXXXX"));
	if (!mountpoint)
		die_errno("mkdtemp");

	mount_partition_device(
		hashmapGetPrintf(ictx.opts, NULL, "partition.cache:device"),
		hashmapGetPrintf(ictx.opts, NULL, "partition.cache:type"),
		mountpoint);

	/* Copy OTA update */
	pr_info("Copying OTA update...\n");
	destfile = xasprintf("%s/ota.zip", mountpoint);
	copy_file(srcfile, destfile);
	free(destfile);

	/* Create command file */
	pr_info("Setting up Recovery Console command file...\n");
	cmdline = "--update_package=/cache/ota.zip";
	destfile = xasprintf("%s/recovery", mountpoint);
	if (mkdir(destfile, 0777) && errno != EEXIST)
		die_errno("mkdir");
	free(destfile);
	destfile = xasprintf("%s/recovery/command", mountpoint);
	fd = xopen(destfile, O_WRONLY | O_CREAT | O_TRUNC);
	free(destfile);
	xwrite(fd, cmdline, strlen(cmdline));
	xclose(fd);
	umount(mountpoint);
	free(mountpoint);
	xhashmapPut(ictx.opts, xstrdup(BASE_REBOOT), xstrdup("recovery"));
}

static struct iago_plugin plugin = {
	.execute = ota_execute
};

struct iago_plugin *ota_init(void)
{
	return &plugin;
}

