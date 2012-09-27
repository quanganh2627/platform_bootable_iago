/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef IAGO_SYSLINUX_H
#define IAGO_SYSLINUX_H

typedef enum {
    SYSLINUX_NO_ERROR = 0,
    SYSLINUX_EXEC_NOT_FOUND,
    SYSLINUX_FILES_NOT_FOUND,
    SYSLINUX_INVALID_INPUT,
    SYSLINUX_FSCK_ERROR,
    SYSLINUX_MKFS_ERROR,
    SYSLINUX_INSTALL_ERROR,
    SYSLINUX_COPY_ERROR,
    SYSLINUX_CONFIG_ERROR,
    SYSLINUX_UNKNOWN_ERROR,

    SYSLINUX_ERROR_MAX = SYSLINUX_UNKNOWN_ERROR
} syslinux_errno;

struct syslinux_cfg_entries_t {
    /* name of entry */
    char* name;

    /* description to be displayed on menu */
    char* description;

    /* com32 module name */
    char* com32_name;

    /* disk number, -1 for 'current' */
    int disk_num;

    /* partition number */
    int part_num;

    /* additional kernel parameters. can be NULL. */
    char* addl_kernel_param;
};

struct syslinux_inst_t {
    /* device node to boot partition, e.g. /dev/block/sda1 */
    char* dev_path;

    /* volume label for boot partition. Can be NULL. */
    char* vol_lbl;

    /* 'misc' partition number */
    int partition_num_misc;

    /* list of menu entries */
    struct syslinux_cfg_entries_t *menu_entries;
    int num_menu_entries;
};

syslinux_errno perform_syslinux_installation(struct syslinux_inst_t *inst);

#endif
