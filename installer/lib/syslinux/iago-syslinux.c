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

#define LOG_TAG "iago-installer-syslinux"

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

#include <cutils/config_utils.h>
#include <cutils/log.h>
#include <cutils/misc.h>

#include <util.h>
#include "iago-syslinux.h"

#define MKDOSFS_BIN         "/system/bin/newfs_msdos"
#define FSCK_MSDOS_BIN      "/system/bin/fsck_msdos"

#define TMP_PATH            "/tmp"

/* SYSLINUX related */
#define BOOTLOADER_PATH     TMP_PATH "/bootloader"
#define SYSLINUX_BIN        "/installmedia/images/syslinux/bin/android_syslinux"
#define SYSLINUX_FILES_PATH "/installmedia/images/syslinux/files"
#define SYSLINUX_CFG_TEM_FN SYSLINUX_FILES_PATH "/syslinux.cfg"
#define SYSLINUX_CFG_FN     BOOTLOADER_PATH "/syslinux.cfg"

static int
do_vfat_fsck(struct syslinux_inst_t *inst)
{
    int rv;
    int pass = 1;

    /* copied from /system/vold/Fat.cpp */
    ALOGI("Running fsck_msdos... This MAY take a while.");
    do {
        rv = exec_cmd(FSCK_MSDOS_BIN, "-p", "-f", inst->dev_path, NULL);

        switch(rv) {
        case 0:
            ALOGI("Filesystem check completed OK");
            return 0;

        case 2:
            ALOGE("Filesystem check failed (not a FAT filesystem)");
            return -1;

        case 4:
            if (pass++ <= 3) {
                ALOGW("Filesystem modified - rechecking (pass %d)",
                        pass);
                continue;
            }
            ALOGE("Failing check after too many rechecks");
            return -1;

        default:
            ALOGE("Filesystem check failed (unknown exit code %d)", rv);
            return -1;
        }
    } while (0);

    return -1;
}

static int
do_create_vfat_fs(struct syslinux_inst_t *inst)
{
    int rv = -1;

    if (inst->vol_lbl) {
        rv = exec_cmd(MKDOSFS_BIN, "-L", inst->vol_lbl, inst->dev_path, NULL);
    } else {
        rv = exec_cmd(MKDOSFS_BIN, inst->dev_path, NULL);
    }

    return rv;
}

static int
do_install_syslinux(struct syslinux_inst_t *inst)
{
    int rv = -1;

    /* executing syslinux to installer bootloader */
    if ((rv = exec_cmd(SYSLINUX_BIN, "--install", inst->dev_path, NULL)) < 0)
        return -1;
    if (rv) {
        ALOGE("Error while running syslinux: %d", rv);
        return -1;
    }

    return rv;
}

static int
do_copy_syslinux_files()
{
    int len, rv = -1;
    DIR* dirp;
    struct dirent *de;
    char* src_fp = NULL;
    char* dst_fp = NULL;
    FILE *dst_fd;
    void *data = NULL;
    unsigned int data_sz;

    /* Copy files there */
    dirp = opendir(SYSLINUX_FILES_PATH);
    while (dirp && (de = readdir(dirp))) {
        if (de->d_type == DT_REG) {
            if (asprintf(&src_fp, "%s/%s", SYSLINUX_FILES_PATH, de->d_name) == -1) {
                ALOGE("Error constructing source filename for '%s'", de->d_name);
                goto fail;
            }
            if (asprintf(&dst_fp, "%s/%s", BOOTLOADER_PATH, de->d_name) == -1) {
                ALOGE("Error constructing destination filename for '%s'", de->d_name);
                goto fail;
            }

            /* load file first using cutils' load_file() */
            data = load_file(src_fp, &data_sz);

            if (!data) {
                ALOGE("Error reading '%s': %s", src_fp, strerror(errno));
                goto fail;
            }

            /* write to destination manually... */
            dst_fd = fopen(dst_fp, "w");
            if (!dst_fd) {
                ALOGE("Error creating file '%s': %s", dst_fp, strerror(errno));
                goto fail;
            }

            len = write(fileno(dst_fd), data, data_sz);
            if (len < 0) {
                ALOGE("Error writing to file '%s': %s", dst_fp, strerror(errno));
                goto fail;
            }

            /* clean up per iteration */
            fclose(dst_fd); dst_fd = NULL;
            free(data);     data = NULL;
            free(src_fp);   src_fp = NULL;
            free(dst_fp);   dst_fp = NULL;
        }
    }
    closedir(dirp);

    rv = 0;

fail:
    if (dst_fd) fclose(dst_fd);
    if (src_fp) free(src_fp);
    if (dst_fp) free(dst_fp);
    if (data)   free(data);
    return rv;
}

static int
do_syslinux_config(struct syslinux_inst_t *inst)
{
    int i, len, rv = -1;
    FILE *dst_fd = NULL;
    void *data = NULL;
    unsigned int data_sz;

    /* Process SYSLINUX config file */
    data = load_file(SYSLINUX_CFG_TEM_FN, &data_sz);
    if (!data) {
        ALOGE("Error reading '%s': %s", SYSLINUX_CFG_TEM_FN, strerror(errno));
        goto fail;
    }
    dst_fd = fopen(SYSLINUX_CFG_FN, "w");
    if (!dst_fd) {
        ALOGE("Error creating file '%s': %s", SYSLINUX_CFG_FN, strerror(errno));
        goto fail;
    }

    /* Write the initial config template */
    len = write(fileno(dst_fd), data, data_sz);
    if (len < 0) {
        ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
        goto fail;
    }

    /* Partition number to BCB */
    len = fprintf(dst_fd, "menu androidcommand %d\n\n", inst->partition_num_misc);
    if (len < 0) {
        ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
        goto fail;
    }

    /* Write boot menu entries */
    for (i = 0; i < inst->num_menu_entries; i++) {
        if (inst->menu_entries[i].disk_num != -1) {
            /* write with disk number */
            len = fprintf(dst_fd,
                    "label %s\n\tmenu label ^%s\n\tcom32 %s\n\tappend %d %d %s\n\n",
                    inst->menu_entries[i].name,
                    inst->menu_entries[i].description,
                    inst->menu_entries[i].com32_name,
                    inst->menu_entries[i].disk_num,
                    inst->menu_entries[i].part_num,
                    inst->menu_entries[i].addl_kernel_param ?
                        inst->menu_entries[i].addl_kernel_param : ""
                  );
        } else {
            /* using current disk, write 'current' instead of disk number */
            len = fprintf(dst_fd,
                    "label %s\n\tmenu label ^%s\n\tcom32 %s\n\tappend current %d %s\n\n",
                    inst->menu_entries[i].name,
                    inst->menu_entries[i].description,
                    inst->menu_entries[i].com32_name,
                    inst->menu_entries[i].part_num,
                    inst->menu_entries[i].addl_kernel_param ?
                        inst->menu_entries[i].addl_kernel_param : ""
                  );
        }

        if (len < 0) {
            ALOGE("Error writing to file '%s' (%s)", SYSLINUX_CFG_FN, strerror(errno));
            goto fail;
        }
    }

    rv = 0;

fail:
    if (dst_fd) fclose(dst_fd);
    if (data)   free(data);
    return rv;
}

/* Perform installation of SYSLINUX onto boot partition. */
/* Return 0 if succeeds, fails if other numbers.         */
syslinux_errno
perform_syslinux_installation(struct syslinux_inst_t *inst)
{
    syslinux_errno rv = SYSLINUX_NO_ERROR;
    int err;

    /* check for file existence first */
    if (access(SYSLINUX_CFG_TEM_FN, R_OK)) {
        ALOGE("Error: %s has no read access or does not exist", SYSLINUX_CFG_TEM_FN);
        rv = SYSLINUX_FILES_NOT_FOUND;
        goto fail;
    }
    if (access(SYSLINUX_BIN, R_OK | X_OK)) {
        ALOGE("Error: %s has no read/execution access or does not exist", SYSLINUX_BIN);
        rv = SYSLINUX_EXEC_NOT_FOUND;
        goto fail;
    }

    /* Need 'misc' partition if this writes the syslinux config file */
    if (inst->num_menu_entries && !inst->partition_num_misc) {
        ALOGE("Invalid partition number for 'misc' partition.");
        rv = SYSLINUX_INVALID_INPUT;
        goto fail;
    }

    /* Create a new VFAT filesystem on boot partition, and do fsck */
    if (do_create_vfat_fs(inst)) {
        ALOGE("Error creating filesystem on boot partition.");
        rv = SYSLINUX_MKFS_ERROR;
        goto fail;
    }

    if (do_vfat_fsck(inst)) {
        ALOGE("Error checking filesystem on boot partition.");
        rv = SYSLINUX_FSCK_ERROR;
        goto fail;
    }

    if (do_install_syslinux(inst)) {
        ALOGE("Error installing SYSLINUX onto partition");
        rv = SYSLINUX_INSTALL_ERROR;
        goto fail;
    }

    if (mkdir(BOOTLOADER_PATH, 0755)) {
        ALOGE("Error creating directory '%s'", BOOTLOADER_PATH);
        rv = SYSLINUX_COPY_ERROR;
        goto fail;
    }

    /* Mount boot partition to copy files */
    if (mount(inst->dev_path, BOOTLOADER_PATH, "vfat", 0, NULL)) {
        ALOGE("Error mounting %s on %s as vfat", inst->dev_path, BOOTLOADER_PATH);
        rv = SYSLINUX_COPY_ERROR;
        goto fail;
    }

    if (do_copy_syslinux_files()) {
        umount(BOOTLOADER_PATH);
        ALOGE("Error copying files onto boot partition.");
        rv = SYSLINUX_COPY_ERROR;
        goto fail;
    }

    /* Populate SYSLINUX config file */
    if (inst->num_menu_entries) {
        err = do_syslinux_config(inst);
        if (err) {
            ALOGE("Error populating syslinux configuration file.");
            rv = SYSLINUX_CONFIG_ERROR;
            goto fail;
        }
    }

fail:
    /* force sync, umount and clean-up* */
    sync();
    umount(BOOTLOADER_PATH);
    rmdir(BOOTLOADER_PATH);

    if (rv == SYSLINUX_NO_ERROR) {
        ALOGI("Installation of SYSLINUX onto boot partition was successful.");
    } else {
        ALOGE("ERROR: cannot install SYSLINUX onto boot partition.");
    }

    return rv;
}
