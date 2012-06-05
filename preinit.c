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

/* Pre-init: Find and mount the installation media, and then execl()
 * the real Init process */

#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

#include <cutils/klog.h>

#define INSTALL_MOUNT   "/installmedia"
#define TMP_NODE        "/dev/__iago_blkdev"

#define dbg(fmt, ...)       KLOG_ERROR("iago", "%s(): " fmt, __func__, ##__VA_ARGS__)
#define dbg_perror(x)       dbg("%s: %s\n", x, strerror(errno))

int is_iso9660(const char *path)
{
    int fd;
    char buf[5];
    int ret = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        dbg_perror("open");
        return 0;
    }

    if (lseek(fd, 16 * 2048 + 1, SEEK_SET) < 0) {
        dbg_perror("lseek");
        goto out;
    }
    if (read(fd, buf, 5) != 5) {
        dbg_perror("read");
        goto out;
    }
    if (!memcmp(buf, "CD001", 5))
        ret = 1;
out:
    close(fd);
    return ret;
}


int is_install_media(char *name)
{
    char pathbuf[PATH_MAX];
    char devinfo[PATH_MAX];
    int fd;
    char *minor_s;
    dev_t dev;
    int ret = 0;
    struct stat statbuf;

    dbg("------> examining %s\n", name);

    /* Read the "dev" node in the sysfs dir which has major:minor
     * and create a temp device node to work with */
    snprintf(pathbuf, sizeof(pathbuf), "%s/dev", name);
    fd = open(pathbuf, O_RDONLY);
    if (fd < 0) {
        dbg_perror("open");
        return 0;
    }
    if (read(fd, devinfo, sizeof(devinfo)) <= 0) {
        dbg_perror("read");
        return 0;
    }
    close(fd);
    minor_s = strchr(devinfo, ':');
    if (!minor_s)
        return 0;
    *minor_s = '\0';
    minor_s++;

    dev = makedev(atoi(devinfo), atoi(minor_s));

    if (mknod(TMP_NODE, S_IFBLK, dev)) {
        dbg_perror("mknod");
        return 0;
    }

    if (!is_iso9660(TMP_NODE)) {
        dbg("Not an iso9660 volume\n");
        goto out_unlink;
    }

    dbg("Mounting device %d:%d as iso9660\n", major(dev), minor(dev));
    /* Try to mount an iso9660 fs */
    if (mount(TMP_NODE, INSTALL_MOUNT, "iso9660", MS_RDONLY, "")) {
        dbg_perror("mount");
        goto out_unlink;
    }

    /* It mounted - check for cookie */
    if (stat(INSTALL_MOUNT "/iago-cookie", &statbuf)) {
        dbg_perror("stat");
        umount(INSTALL_MOUNT);
    } else {
        dbg("'%s' is Iago media\n", pathbuf);
        ret = 1;
    }

out_unlink:
    unlink(TMP_NODE);

    return ret;
}


int mount_cdrom(void)
{
    int ret = -1;
    DIR *dir = NULL;
    DIR *dir2 = NULL;
    char path[PATH_MAX];

    dir = opendir("/sys/block");
    if (!dir) {
        dbg_perror("opendir");
        goto out;
    }
    while (1) {
        struct dirent *dp = readdir(dir);
        if (!dp)
            break;
        if (!strncmp(dp->d_name, ".", 1) ||
                !strncmp(dp->d_name, "ram", 3) ||
                !strncmp(dp->d_name, "loop", 4)) {
            continue;
        }

        snprintf(path, sizeof(path), "/sys/block/%s/", dp->d_name);
        if (is_install_media(path)) {
            ret = 0;
            goto out;
        }
        if ( (dir2 = opendir(path)) == NULL) {
            dbg_perror("opendir");
            goto out;
        }
        while (1) {
            struct dirent *dp2 = readdir(dir2);
            if (!dp2)
                break;
            if (strncmp(dp2->d_name, dp->d_name, strlen(dp->d_name)))
                continue;
            snprintf(path, sizeof(path), "/sys/block/%s/%s/",
                    dp->d_name, dp2->d_name);
            if (is_install_media(path)) {
                ret = 0;
                goto out;
            }
        }
        closedir(dir2);
        dir2 = NULL;
    }

out:
    if (dir)
        closedir(dir);
    if (dir2)
        closedir(dir2);
    return ret;
}


int main(void)
{
    int count = 15;

    mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755");
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    klog_init();
    klog_set_level(8);

    while (1) {
        if (!mount_cdrom())
            break;

        if (count--) {
            sleep(2);
            dbg("Iago media not found, trying again...\n");
            continue;
        }
        dbg("Couldn't find Iago media!\n");
        exit(1); /* will result in kernel panic */
    }

    umount("/dev");
    umount("/sys");

    /* ok, start the real init */
    return execl("/init2", "init", NULL);
}
