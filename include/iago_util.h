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


#ifndef IAGO_UTIL_H
#define IAGO_UTIL_H

#include <stdlib.h>
#include <ctype.h>
#include <cutils/hashmap.h>
#include <iniparser.h>
#include <cutils/list.h>

#define _unused __attribute__((unused))
#define _noreturn __attribute__((noreturn))

/* Attribute specification and -Werror prevents most security shenanigans with
 * these functions */
int execute_command(const char *fmt, ...) __attribute__((format(printf,1,2)));
int execute_command_data(void *data, unsigned sz, const char *fmt, ...)
		__attribute__((format(printf,3,4)));

#define die(fmt, ...) __die("%s:%s:%d " fmt, __FILE__, __func__, __LINE__, \
		##__VA_ARGS__)

#define die_errno(fmt, ...) die(fmt ": %s", ##__VA_ARGS__, strerror(errno))

void __die(const char *fmt, ...) _noreturn
		__attribute__((format(printf,1,2)));

void mount_partition_device(const char *device, const char *type, char *mountpoint);
int is_valid_blkdev(const char *node);

/* Fails assertion in case of errors */
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf,1,2)));
void *xmalloc(size_t size);
off_t xlseek(int fd, off_t offset, int whence);
ssize_t xread(int fd, void *buf, size_t count);
int xopen(const char *pathname, int flags);
void xclose(int fd);
long int xatol(const char *nptr);
long long int xatoll(const char *nptr);
ssize_t xwrite(int fd, const void *buf, size_t count);

/* Volume operations */
void ext4_filesystem_checks(const char *device, size_t footer);
void vfat_filesystem_checks(const char *device);

bool str_equals(void *keyA, void *keyB);
int str_hash(void *key);
void hashmap_add_dictionary(Hashmap *h, dictionary *d);
void hashmap_destroy(Hashmap *h);
void string_list_iterate(char *list, bool (*cb)(char *entry, int index,
			void *context), void *context);

char *hashmapGetPrintf(Hashmap *h, void *dfl, const char *fmt, ...)
		__attribute__((format(printf,3,4)));
char *xhashmapPut(Hashmap *h, void *key, void *value);
void hashmap_dump(Hashmap *h);

void copy_file(const char *src, const char *dest);
void dd(const char *src, const char *dest);
void append_file(const char *src, const char *dest);

void write_opts(Hashmap *h, const char *filename);
void put_string(int fd, const char *fmt, ...)
		__attribute__((format(printf,2,3)));


char *read_sysfs(const char *fmt, ...)
		__attribute__((format(printf,1,2)));
int64_t read_sysfs_int(const char *fmt, ...)
		__attribute__((format(printf,1,2)));

void string_list_append(char **list, char *entry);
uint64_t get_volume_size(const char *device);
int make_ext4fs_nowipe(const char *filename, int64_t len,
                char *mountpoint, struct selabel_handle *sehnd);

void ui_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
char *ui_option_get(const char *question, struct listnode *list);
bool ui_ask(const char *question, bool dfl);
void option_list_free(struct listnode *list);
void ui_pause(void);

#endif
