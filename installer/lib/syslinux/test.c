/*
 * This is a test program for libiago_syslinux.
 * This installs syslinux onto a partition with menu entries
 * similiar to what the static installer, but with slightly
 * different description.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#include <iago-syslinux.h>

#define TMP_PATH "/tmp"

int
main(int argc, char *argv[])
{
    struct syslinux_inst_t *inst;
    syslinux_errno rv;

    inst = calloc(1, sizeof(*inst));
    if (!inst) {
        printf("ERROR allocating memory\n");
        return -1;
    }

    if (argc < 2) {
        printf("Usage: %s device_path\n", argv[0]);
        return 1;
    }

    /* Mount /tmp and install SYSLINUX onto boot partition */
    if (mount("none", TMP_PATH, "tmpfs", 0, NULL)) {
        printf("ERROR mounting /tmp as tmpfs");
        return -1;
    }

    asprintf(&(inst->dev_path), "%s", argv[1]);
    inst->vol_lbl  = NULL;
    inst->partition_num_misc = 6;

    inst->num_menu_entries = 3;
    inst->menu_entries = calloc(3, sizeof(*(inst->menu_entries)));

    asprintf(&(inst->menu_entries[0].name), "%s", "boot");
    asprintf(&(inst->menu_entries[0].description), "%s", "Boot to Android");
    asprintf(&(inst->menu_entries[0].com32_name), "%s", "android.c32");
    asprintf(&(inst->menu_entries[0].addl_kernel_param), "%s", "android.instpart=10");
    inst->menu_entries[0].disk_num = -1;
    inst->menu_entries[0].part_num = 2;

    asprintf(&(inst->menu_entries[1].name), "%s", "recovery");
    asprintf(&(inst->menu_entries[1].description), "%s", "Recovery");
    asprintf(&(inst->menu_entries[1].com32_name), "%s", "android.c32");
    inst->menu_entries[1].disk_num = -1;
    inst->menu_entries[1].part_num = 3;
    inst->menu_entries[1].addl_kernel_param = NULL;

    asprintf(&(inst->menu_entries[2].name), "%s", "droidboot");
    asprintf(&(inst->menu_entries[2].description), "%s", "Droidboot");
    asprintf(&(inst->menu_entries[2].com32_name), "%s", "android.c32");
    inst->menu_entries[2].disk_num = -1;
    inst->menu_entries[2].part_num = 5;
    inst->menu_entries[2].addl_kernel_param = NULL;

    rv = perform_syslinux_installation(inst);
    if (rv == SYSLINUX_NO_ERROR) {
        printf("Installation of SYSLINUX onto boot partition was successful.\n");
    } else {
        printf("Failed to install syslinux onto partition, status = %d\n", rv);
    }

    free(inst->menu_entries[0].name);
    free(inst->menu_entries[0].description);
    free(inst->menu_entries[0].com32_name);
    free(inst->menu_entries[0].addl_kernel_param);

    free(inst->menu_entries[1].name);
    free(inst->menu_entries[1].description);
    free(inst->menu_entries[1].com32_name);

    free(inst->menu_entries[2].name);
    free(inst->menu_entries[2].description);
    free(inst->menu_entries[2].com32_name);

    free(inst->dev_path);
    free(inst);

    umount(TMP_PATH);

    return 0;
}
