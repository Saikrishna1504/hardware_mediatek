/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "libperfctl_vendor"

#include <log/log.h>

uint64_t earaNotifyCVJobBegin(uint64_t jobId, uint64_t jobPriority, uint64_t* result) {
    ALOGD("[%s]: jobId: %llu, jobPriority: %llu, result: %p", __func__, jobId, jobPriority, result);
    return 0;
}

uint64_t earaNotifyCVJobEnd(uint64_t jobId, uint64_t timestamp, uint32_t* status) {
    ALOGD("[%s]: jobId: %llu, timestamp: %llu, status: %p", __func__, jobId, timestamp, status);
    return 0;
}

void earaGetUsage(uint32_t param1, uint32_t* usage, uint32_t* status) {
    ALOGD("[%s]: param1: %u, usage: %p, status: %p", __func__, param1, usage, status);
}

void earaNotifyJobBegin(uint32_t jobId, uint64_t jobData, int64_t* param3, int64_t* param4) {
    ALOGD("[%s]: jobId: %u, jobData: %llu, param3: %p, param4: %p", __func__, jobId, jobData,
          param3, param4);
}

uint64_t earaNotifyJobEnd(uint32_t jobId, uint64_t timestamp, uint32_t* status, uint32_t flags,
                          int64_t* inputData, int64_t* outputData, uint64_t context,
                          int64_t* result) {
    ALOGD("[%s]: jobId: %u, timestamp: %llu, status: %p, flags: %u, inputData: "
          "%p, outputData: %p, context: %llu, result: %p",
          __func__, jobId, timestamp, status, flags, inputData, outputData, context, result);
    return 0;
}

void fbcNotifyTouch(int touchEvent) {
    ALOGD("[%s]: touchEvent: %d", __func__, touchEvent);
}

void xgfGetCamApkPid(uint32_t pid, uint32_t apkId, uint32_t flag) {
    ALOGD("[%s]: pid: %u, apkId: %u, flag: %u", __func__, pid, apkId, flag);
}

void xgfGetCamServerPid(void) {
    ALOGD("[%s]: Get camera server PID", __func__);
}

void xgfGetFPS(int32_t* currentFPS, int32_t* averageFPS) {
    ALOGD("[%s]: currentFPS: %p, averageFPS: %p", __func__, currentFPS, averageFPS);
}

void xgfGetFstbActive(uint64_t status) {
    ALOGD("[%s]: status: %llu", __func__, status);
}

void xgfWaitFstbActive(void) {
    ALOGD("%s called", __func__);
}

uint32_t xgfGetCmd(uint64_t* param) {
    ALOGD("[%s]: param: %p", __func__, param);
    return 0;
}
