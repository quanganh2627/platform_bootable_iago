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
#include <fcntl.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include <iago.h>
#include <iago_util.h>
#include "iago_private.h"

static void finalizer_cli(void)
{
	// sdcard device node, append to partitions as 'special'
}

static bool write_props_cb(void *k, void *v, void *context)
{
	int propsfd = (int)context;
	char *key = k;
	char *value = v;

	put_string(propsfd, "%s=%s\n", key, value);
	return true;
}


static void write_install_props(void)
{
	int propsfd;

	propsfd = xopen("/mnt/" PROP_PATH_FACTORY, O_WRONLY | O_CREAT);
	hashmapForEach(ictx.iprops, write_props_cb, (void *)propsfd);
	xclose(propsfd);
}


static void finalizer_execute(void)
{
	char *device, *type;

	pr_info("Finalizing installation...");
	device = hashmapGetPrintf(ictx.opts, NULL, "partition.factory:device");
	type = hashmapGetPrintf(ictx.opts, NULL, "partition.factory:type");

	mount_partition_device(device, type, "/mnt/factory");
	write_install_props();
	umount("/mnt/factory");

	/* Just for info */
	pr_info("androidboot.install_id=%s", hashmapGetPrintf(ictx.opts,
				NULL, INSTALL_ID));
}


static struct iago_plugin plugin = {
	.cli_session = finalizer_cli,
	.execute = finalizer_execute
};


struct iago_plugin *finalizer_init(void)
{
	return &plugin;
}
