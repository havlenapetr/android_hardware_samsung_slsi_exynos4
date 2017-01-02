/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "SecHWCVSync.h"

#define LOG_TAG "SecHWVSync"
#include <cutils/log.h>

#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>

#include <hardware_legacy/uevent.h>

#include <cutils/properties.h>
#include <cutils/log.h>
#include <utils/Timers.h>

struct exynos4_vsync_ctx_t {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
    pthread_t       thread;
    bool            active = false;
    int             signal = SIGINT;
} exynos4_vsync_ctx;

static void thread_init() {
    sigset_t sigSet;
    sigemptyset(&sigSet);
    sigaddset (&sigSet, exynos4_vsync_ctx.signal);
 
    pthread_sigmask (SIG_UNBLOCK, &sigSet, NULL);
 
    struct sigaction act;
    act.sa_handler = NULL;
    sigaction (exynos4_vsync_ctx.signal, &act, NULL);

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);
}

static void handle_vsync_event(struct exynos4_hwc_composer_device_1_t *pdev) {
    if (!pdev->procs)
        return;

    int err = lseek(pdev->vsync_fd, 0, SEEK_SET);
    if (err < 0) {
        ALOGE("error seeking to vsync timestamp: %s", strerror(errno));
        return;
    }

    char buf[4096];
    err = read(pdev->vsync_fd, buf, sizeof(buf));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return;
    }
    buf[sizeof(buf) - 1] = '\0';

    errno = 0;
    uint64_t timestamp = strtoull(buf, NULL, 0);
    if (!errno)
        pdev->procs->vsync(pdev->procs, 0, timestamp);
}

static void *hw_vsync_thread(void *data) {
    struct exynos4_hwc_composer_device_1_t *pdev =
            (struct exynos4_hwc_composer_device_1_t *)data;
    char uevent_desc[4096];

    memset(uevent_desc, 0, sizeof(uevent_desc));
    
    thread_init();

    uevent_init();

    char temp[4096];
    int err = read(pdev->vsync_fd, temp, sizeof(temp));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return NULL;
    }

    struct pollfd fds[2];
    fds[0].fd = pdev->vsync_fd;
    fds[0].events = POLLPRI;
    fds[1].fd = uevent_get_fd();
    fds[1].events = POLLIN;

    while (true) {
        int err = poll(fds, 2, -1);
        if (err > 0) {
            if (fds[0].revents & POLLPRI) {
                handle_vsync_event(pdev);
            }
            else if (fds[1].revents & POLLIN) {
            }
        } else if (err == -1) {
            if (errno == EINTR)
                break;
            ALOGE("error in vsync thread: %s", strerror(errno));
        }
    }

    return NULL;
}

static nsecs_t get_period(struct exynos4_hwc_composer_device_1_t *pdev) {
    nsecs_t period;

    pthread_mutex_lock(&exynos4_vsync_ctx.mutex);
    while (!exynos4_vsync_ctx.active) {
        pthread_cond_wait(&exynos4_vsync_ctx.cond, &exynos4_vsync_ctx.mutex);
    }
    period = pdev->vsync_period;
    pthread_mutex_unlock(&exynos4_vsync_ctx.mutex);

    return period;
}

extern "C" int clock_nanosleep(clockid_t clock_id, int flags,
                           const struct timespec *request,
                           struct timespec *remain);

static int nanosleep(struct timespec* tp_sleep) {
    int err;

    do {
        err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, tp_sleep, NULL);
    } while (err<0 && errno == EINTR);

    return err;
}

static void *sw_vsync_thread(void *data) {
    struct exynos4_hwc_composer_device_1_t *pdev =
            (struct exynos4_hwc_composer_device_1_t *) data;
    struct timespec tp, tp_next, tp_sleep;
    nsecs_t now = 0, next_vsync = 0, next_fake_vsync = 0, sleep = 0;
    tp_sleep.tv_sec = tp_sleep.tv_nsec = 0;

    thread_init();

    for (nsecs_t period = get_period(pdev); errno != EINTR; period = get_period(pdev)) {
        now = systemTime(CLOCK_MONOTONIC);
        next_vsync = next_fake_vsync;
        sleep = next_vsync - now;
        if (sleep < 0) {
            // we missed, find where the next vsync should be
            sleep = (period - ((now - next_vsync) % period));
            next_vsync = now + sleep;
        }
        next_fake_vsync = next_vsync + period;

        tp_sleep.tv_sec  = next_vsync / 1000000000;
        tp_sleep.tv_nsec = next_vsync % 1000000000;

        int err = nanosleep(&tp_sleep);
        if (err == 0 && pdev->procs && pdev->procs->vsync) {
            pdev->procs->vsync(pdev->procs, 0, next_vsync);
        }
    }
    return NULL;
}

int exynos4_vsync_set_enabled(struct exynos4_hwc_composer_device_1_t *dev, int enabled) {
    pthread_mutex_lock(&exynos4_vsync_ctx.mutex);
    if (exynos4_vsync_ctx.active) {
        pthread_mutex_unlock(&exynos4_vsync_ctx.mutex);
        return 0;
    }
    if (dev->fd >= 0) {
        ioctl(dev->fd, S3CFB_SET_VSYNC_INT, &enabled);
    }
    exynos4_vsync_ctx.active = !!enabled;
    pthread_mutex_unlock(&exynos4_vsync_ctx.mutex);
    return pthread_cond_signal(&exynos4_vsync_ctx.cond);
}

int exynos4_vsync_init(struct exynos4_hwc_composer_device_1_t *dev) {
    sigset_t sigSet;
    void* (*vsync_routine)(void*);

    dev->vsync_fd = open("/sys/devices/platform/exynos4-fb.0/vsync", O_RDONLY);
    if (dev->vsync_fd >= 0) {
        vsync_routine = &hw_vsync_thread;
    } else {
        ALOGI("failed to open vsync attribute, use SW vsync");
        vsync_routine = &sw_vsync_thread;
    }

    sigemptyset(&sigSet);
    sigaddset(&sigSet, exynos4_vsync_ctx.signal);
    pthread_sigmask (SIG_BLOCK, &sigSet, NULL);
    return pthread_create(&exynos4_vsync_ctx.thread, NULL, vsync_routine, dev);
}

int exynos4_vsync_deinit(struct exynos4_hwc_composer_device_1_t *dev) {
    ALOGI("Stopping vsync thread");
    ALOGI("Waiting for vsync thread end");
    int ret = pthread_kill(exynos4_vsync_ctx.thread, exynos4_vsync_ctx.signal);
    ALOGI("Vsync thread kill %d", ret);
    ALOGI("Vsync thread ended");

    if(dev->vsync_fd >= 0) {
        close(dev->vsync_fd);
        dev->vsync_fd = -1;
    }
    return ret;
}
