//
// SPDX-FileCopyrightText: The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
//

#define LOG_TAG "libaedv"

#include <log/log.h>

int aee_switch_ftrace(int status) {
    ALOGD("[%s]: status: %d", __func__, status);
    return 0;
}

int aee_system_exception(const char *lib, const char *path, unsigned int options, const char *log_msg) {
    ALOGD("[%s]: lib: %s, path: %s, options: %u, log_msg: %s", __func__, lib, path, options, log_msg);
    return 0;
}

int aee_system_warning(const char *lib, const char *path, unsigned int options, const char *log_msg) {
    ALOGD("[%s]: lib: %s, path: %s, options: %u, log_msg: %s", __func__, lib, path, options, log_msg);
    return 0;
}

int aee_modem_warning(const char *lib,const char* path,unsigned int options, const char *log_msg,  const char *ver) {
    ALOGD("[%s]: lib: %s, path: %s, options: %u, log_msg: %s, ver: %s", __func__, lib, path, options, log_msg, ver);
    return 0;
}

int aee_log_msg(const char *lib, const char *path, unsigned int options, const char *log_msg, const char *ver) {
    ALOGD("[%s]: lib: %s, path: %s, options: %u, log_msg: %s, ver: %s", __func__, lib, path, options, log_msg, ver);
    return 0;
}
