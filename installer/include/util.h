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

#ifndef IAGO_UTIL_H
#define IAGO_UTIL_H

/* from bootable/diskinstaller/installer.c */
inline int
exec_cmd(const char *cmd, ...) /* const char *arg, ...) */
{
    va_list ap;
    int size = 0;
    char *str;
    char *outbuf;
    int rv;

    /* compute the size for the command buffer */
    size = strlen(cmd) + 1;
    va_start(ap, cmd);
    while ((str = va_arg(ap, char *))) {
        size += strlen(str) + 1;  /* need room for the space separator */
    }
    va_end(ap);

    if (!(outbuf = malloc(size + 1))) {
        ALOGE("Can't allocate memory to exec cmd");
        return -1;
    }

    /* this is a bit inefficient, but is trivial, and works */
    strcpy(outbuf, cmd);
    va_start(ap, cmd);
    while ((str = va_arg(ap, char *))) {
        strcat(outbuf, " ");
        strcat(outbuf, str);
    }
    va_end(ap);

    ALOGI("Executing: %s", outbuf);
    rv = system(outbuf);
    if (rv < 0) {
        ALOGI("Error while trying to execute '%s'", cmd);
        rv = -1;
        goto exec_cmd_end;
    }
    rv = WEXITSTATUS(rv);
    ALOGI("Done executing %s (%d)", outbuf, rv);

exec_cmd_end:
    free(outbuf);
    return rv;
}

#endif
