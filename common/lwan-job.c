/*
 * lwan - simple web server
 * Copyright (c) 2012, 2013 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*lwan job 低优先级后台线程*/

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#include "lwan.h"
#include "lwan-status.h"
#include "list.h"

struct job {
    struct list_node jobs;
    bool (*cb)(void *data);
    void *data;
};

static pthread_t self;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool running = false;      // job 线程开挂标志
static struct list_head jobs;

// 修改原来lwan调用sleep的方法，sleep的方式不能响应信号，退出时间比较长
static pthread_mutex_t job_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  job_wait_cond = PTHREAD_COND_INITIALIZER;

static void*
job_thread(void *data __attribute__((unused)))
{
    // 后台处理线程，当程序退出时，原来程序的逻辑用sleep的方式，导致程序延迟退出，这里是已经修复的
    // 已经是已经优化的，已经提交issue： https://github.com/lpereira/lwan/issues/196 
    pthread_mutex_lock(&job_wait_mutex);
    int job_wait_sec = 1;
    
    while (running) {
        bool had_job = false;

        if (LIKELY(!pthread_mutex_lock(&queue_mutex))) {
            struct job *job;

            list_for_each(&jobs, job, jobs)
                had_job |= job->cb(job->data);

            pthread_mutex_unlock(&queue_mutex);
        }


        if (had_job)
            job_wait_sec = 1;
        else if (job_wait_sec <= 15)
            job_wait_sec++;
        
        struct timeval now;
        gettimeofday(&now, NULL);
        struct timespec rgtp = { now.tv_sec + job_wait_sec, now.tv_usec * 1000 };

        pthread_cond_timedwait(&job_wait_cond, &job_wait_mutex, &rgtp);
        //if (UNLIKELY(nanosleep(&rgtp, NULL) < 0)) {
        //    if (errno == EINTR)
        //        sleep(1);
        //}
    }
     pthread_mutex_unlock(&job_wait_mutex);

    return NULL;
}

void lwan_job_thread_init(void)
{
    assert(!running);

    lwan_status_debug("Initializing low priority job thread");

    list_head_init(&jobs);

    running = true;
    if (pthread_create(&self, NULL, job_thread, NULL) < 0)
        lwan_status_critical_perror("pthread_create");

#ifdef SCHED_IDLE
    struct sched_param sched_param = {
        .sched_priority = 0
    };
    // 设置进程调度级别为SCHED_IDLE非常低级别
    if (pthread_setschedparam(self, SCHED_IDLE, &sched_param) < 0)
        lwan_status_perror("pthread_setschedparam");
#endif  /* SCHED_IDLE */
}

void lwan_job_thread_shutdown(void)
{
    lwan_status_debug("Shutting down job thread");

    if (LIKELY(!pthread_mutex_lock(&queue_mutex))) {
        struct job *node, *next;
        int r;

        list_for_each_safe(&jobs, node, next, jobs) {
            list_del(&node->jobs);
            free(node);
        }
        running = false;

        pthread_cond_signal(&job_wait_cond);

        // 等待线程结束
        r = pthread_join(self, NULL);
        if (r) {
            errno = r;
            lwan_status_perror("pthread_join");
        }

        pthread_mutex_unlock(&queue_mutex);
    }
}

void lwan_job_add(bool (*cb)(void *data), void *data)
{
    assert(cb);

    struct job *job = calloc(1, sizeof(*job));
    if (!job)
        lwan_status_critical_perror("calloc");

    job->cb = cb;
    job->data = data;

    if (LIKELY(!pthread_mutex_lock(&queue_mutex))) {
        list_add(&jobs, &job->jobs);
        pthread_mutex_unlock(&queue_mutex);
    } else {
        lwan_status_warning("Couldn't lock job mutex");
        free(job);
    }
}

void lwan_job_del(bool (*cb)(void *data), void *data)
{
    struct job *node, *next;

    assert(cb);

    if (LIKELY(!pthread_mutex_lock(&queue_mutex))) {
        list_for_each_safe(&jobs, node, next, jobs) {
            if (cb == node->cb && data == node->data) {
                list_del(&node->jobs);
                free(node);
            }
        }
        pthread_mutex_unlock(&queue_mutex);
    }
}
