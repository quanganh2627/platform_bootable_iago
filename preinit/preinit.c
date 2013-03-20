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
#include <stdarg.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <regex.h>

#include <cutils/klog.h>

#define DISK_MATCH_REGEX    "^[.]+|(ram|loop)[0-9]+|mmcblk[0-9]+(rpmb|boot[0-9]+)$"
#define INSTALL_MOUNT       "/installmedia"
#define TMP_NODE            "/dev/__iago_blkdev"

#define dbg(fmt, ...)       KLOG_INFO("iago", "%s(): " fmt, __func__, ##__VA_ARGS__)
#define dbg_perror(x)       dbg("%s: %s\n", x, strerror(errno))

static int g_check_vfat;

int put_string(int fd, const char *fmt, ...)
{
    char *buf, *buf_ptr;
    va_list ap;
    ssize_t to_write;

    va_start(ap, fmt);
    if (vasprintf(&buf, fmt, ap) < 0) {
        dbg_perror("vasprintf");
        return -1;
    }

    va_end(ap);

    buf_ptr = buf;
    to_write = strlen(buf);
    while (to_write) {
        ssize_t written = write(fd, buf_ptr, to_write);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            dbg_perror("write");
            free(buf);
            return -1;
        }
        to_write -= written;
        buf_ptr += written;
    }
    free(buf);
    return 0;
}


int seek_and_read(int fd, off_t offset, void *buf, size_t count)
{
    if (lseek(fd, offset, SEEK_SET) < 0) {
        dbg_perror("lseek");
        return -1;
    }
    if (read(fd, buf, count) != (ssize_t)count) {
        dbg_perror("read");
        return -1;
    }
    return 0;
}


int is_expected_fs(const char *path)
{
    int fd;
    unsigned char buf[16];
    int ret = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        dbg_perror("open");
        return 0;
    }

    if (g_check_vfat) {
        unsigned char buf[2];
        /* No consistent 'magic' in FAT32, but some fields
         * have fixed values (always little-endian) in all but
         * very obscure cases */

        /* Check signature */
        if (seek_and_read(fd, 0x1FE, buf, 2))
            goto out;
        if (buf[0] != 0x55 || buf[1] != 0xAA)
            goto out;

        /* Check number of FATs, should be 2 */
        if (seek_and_read(fd, 0x10, buf, 1))
            goto out;
        if (buf[0] != 2)
            goto out;

        /* Check bytes per sector, should be 512 */
        if (seek_and_read(fd, 0x0B, buf, 2))
            goto out;
        if (buf[0] != 0x00 || buf[1] != 0x02)
            goto out;

        ret = 1;
    } else {
        /* Check for ISO9660 magic value */
        if (seek_and_read(fd, 16 * 2048 + 1, buf, 5))
            goto out;

        if (!memcmp(buf, "CD001", 5))
            ret = 1;
    }
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

    if (!is_expected_fs(TMP_NODE)) {
        dbg("Not the right fs type\n");
        goto out_unlink;
    }

    dbg("Mounting device %d:%d\n", major(dev), minor(dev));
    /* Try to mount an iso9660 fs */
    if (mount(TMP_NODE, INSTALL_MOUNT, g_check_vfat ? "vfat" : "iso9660",
                    MS_RDONLY, "")) {
        dbg_perror("mount");
        goto out_unlink;
    }

    /* It mounted - check for cookie */
    if (stat(INSTALL_MOUNT "/iago-cookie", &statbuf)) {
        dbg_perror("stat iago cookie");
        umount(INSTALL_MOUNT);
    } else {
        dbg("'%s' is Iago media\n", pathbuf);
        ret = 1;
    }

out_unlink:
    unlink(TMP_NODE);

    return ret;
}


int mount_device(void)
{
    int ret = -1;
    DIR *dir = NULL;
    DIR *dir2 = NULL;
    char path[PATH_MAX];
    char *name = NULL;
    int fd;
    regex_t diskreg;

    if (regcomp(&diskreg, DISK_MATCH_REGEX, REG_EXTENDED | REG_NOSUB)) {
        dbg_perror("regcomp");
        goto out;
    }

    dir = opendir("/sys/block");
    if (!dir) {
        dbg_perror("opendir");
        goto out;
    }
    while (1) {
        struct dirent *dp = readdir(dir);
        if (!dp)
            goto out;
        name = dp->d_name;

        if (!regexec(&diskreg, name, 0, NULL, 0))
            continue;

        snprintf(path, sizeof(path), "/sys/block/%s/", name);
        if (is_install_media(path))
            goto success;

        if ( (dir2 = opendir(path)) == NULL) {
            dbg_perror("opendir");
            goto out;
        }
        while (1) {
            struct dirent *dp2 = readdir(dir2);
            if (!dp2)
                break;
            if (!regexec(&diskreg, dp2->d_name, 0, NULL, 0))
                continue;
            if (strncmp(dp2->d_name, name, strlen(name)))
                continue;
            snprintf(path, sizeof(path), "/sys/block/%s/%s/",
                    name, dp2->d_name);
            if (is_install_media(path))
                goto success;

        }
        closedir(dir2);
        dir2 = NULL;
    }

success:
    fd = open("/default.prop", O_WRONLY | O_APPEND);
    if (fd < 0) {
        dbg_perror("open");
        goto out;
    }
    ret = put_string(fd, "\nro.iago.media=%s\n", name);
    close(fd);

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
    char *val;

    val = getenv("PREINIT_CHECK_FAT");
    g_check_vfat = !!val;

    mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755");
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    klog_init();
    klog_set_level(8);

    while (1) {
        if (!mount_device())
            break;

        if (count--) {
            sleep(2);
            dbg("Iago media not found, trying again...\n");
            continue;
        }
        KLOG_ERROR("Iago", "Couldn't find installation media!\n");
        exit(1); /* will result in kernel panic */
    }

    umount("/dev");
    umount("/sys");

    /* ok, start the real init */
    return execl("/init2", "init", NULL);
}
