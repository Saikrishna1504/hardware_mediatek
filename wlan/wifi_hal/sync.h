/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>

#ifndef __WIFI_HAL_SYNC_H__
#define __WIFI_HAL_SYNC_H__

class Mutex {
  private:
    pthread_mutex_t mMutex;

  public:
    Mutex() { pthread_mutex_init(&mMutex, NULL); }
    ~Mutex() { pthread_mutex_destroy(&mMutex); }
    int tryLock() { return pthread_mutex_trylock(&mMutex); }
    int lock() { return pthread_mutex_lock(&mMutex); }
    void unlock() { pthread_mutex_unlock(&mMutex); }
};

class Condition {
  private:
    pthread_cond_t mCondition;
    pthread_mutex_t mMutex;

  public:
    Condition() {
        pthread_mutex_init(&mMutex, NULL);
        pthread_cond_init(&mCondition, NULL);
    }
    ~Condition() {
        pthread_cond_destroy(&mCondition);
        pthread_mutex_destroy(&mMutex);
    }

    int wait() { return pthread_cond_wait(&mCondition, &mMutex); }

    int wait(struct timespec abstime) {
        struct timeval now;
        int status;

        gettimeofday(&now, NULL);

        abstime.tv_sec += now.tv_sec;
        if (((abstime.tv_nsec + now.tv_usec * 1000) > 1000 * 1000 * 1000) ||
            (abstime.tv_nsec + now.tv_usec * 1000 < 0)) {
            abstime.tv_sec += 1;
            abstime.tv_nsec += now.tv_usec * 1000;
            abstime.tv_nsec -= 1000 * 1000 * 1000;
        } else {
            abstime.tv_nsec += now.tv_usec * 1000;
        }
        status = pthread_cond_timedwait(&mCondition, &mMutex, &abstime);
        return status;
    }

    void signal() { pthread_cond_signal(&mCondition); }
};

#endif