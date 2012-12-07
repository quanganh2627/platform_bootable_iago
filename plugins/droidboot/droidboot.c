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


#include <iago.h>
#include <iago_util.h>

void droidboot_cli(void)
{
	char *plist;

	if (!ui_ask("Install Fastboot?", true))
		return;

	plist = hashmapGetPrintf(ictx.opts, NULL, BASE_PTN_LIST);
	string_list_append(&plist, "fastboot");
	xhashmapPut(ictx.opts, xstrdup(BASE_PTN_LIST), plist);

	plist = hashmapGetPrintf(ictx.opts, NULL, BASE_BOOT_LIST);
	string_list_append(&plist, "fastboot");
	xhashmapPut(ictx.opts, xstrdup(BASE_BOOT_LIST), plist);
}

static struct iago_plugin plugin = {
	.cli_session = droidboot_cli,
};

struct iago_plugin *droidboot_init(void)
{
	return &plugin;
}

