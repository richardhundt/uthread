/* A radically stripped down version of the libuv threads code, which has the
 * following copyright notice:
 *
 * Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>

#include <sys/time.h>

#include "uthread.h"

#undef NANOSEC
#define NANOSEC ((uint64_t) 1e9)

static void* uthread_start(void *arg) {
  struct uthread_ctx *ctx_p;
  struct uthread_ctx ctx;

  ctx_p = arg;
  ctx = *ctx_p;
  free(ctx_p);
  ctx.entry(ctx.arg);

  return 0;
}

int uthread_join(uthread_t *tid) {
  return -pthread_join(*tid, NULL);
}

uthread_t* uthread_create(void (*entry)(void *arg), void *arg) {
  struct uthread_ctx* ctx;
  uthread_t* tid;
  int err;

  tid = malloc(sizeof(*tid));
  if (tid == NULL) return NULL;

  ctx = malloc(sizeof(*ctx));
  if (ctx == NULL) return NULL;

  ctx->entry = entry;
  ctx->arg = arg;

  err = pthread_create(tid, NULL, uthread_start, ctx);

  if (err) {
    free(ctx);
    free(tid);
  }

  return err ? NULL : tid;
}

void uthread_destroy(uthread_t* tid) {
  if (tid != NULL) {
    uthread_join(tid);
    free(tid);
  }
}

unsigned long uthread_self(void) {
  return (unsigned long) pthread_self();
}

static int umutex_init(umutex_t* mutex) {
#if defined(NDEBUG) || !defined(PTHREAD_MUTEX_ERRORCHECK)
  return -pthread_mutex_init(mutex, NULL);
#else
  pthread_mutexattr_t attr;
  int err;

  if (pthread_mutexattr_init(&attr))
    abort();

  if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK))
    abort();

  err = pthread_mutex_init(mutex, &attr);

  if (pthread_mutexattr_destroy(&attr))
    abort();

  return -err;
#endif
}

umutex_t* umutex_create(void) {
  umutex_t* mutex = (umutex_t*)calloc(1, sizeof(umutex_t));
  if (mutex == NULL) return NULL;
  if (umutex_init(mutex)) {
    free(mutex);
    return NULL;
  }
  return mutex;
}

void umutex_destroy(umutex_t* mutex) {
  int rc = pthread_mutex_destroy(mutex);
  free(mutex);
  if (rc) abort();
}

void umutex_lock(umutex_t* mutex) {
  if (pthread_mutex_lock(mutex))
    abort();
}

int umutex_trylock(umutex_t* mutex) {
  int err;
  err = pthread_mutex_trylock(mutex);
  if (err && err != EBUSY && err != EAGAIN)
    abort();

  return -err;
}

void umutex_unlock(umutex_t* mutex) {
  if (pthread_mutex_unlock(mutex))
    abort();
}

#if defined(__APPLE__) && defined(__MACH__)

static int ucond_init(ucond_t* cond) {
  return -pthread_cond_init(cond, NULL);
}

#else /* !(defined(__APPLE__) && defined(__MACH__)) */

static int ucond_init(ucond_t* cond) {
  pthread_condattr_t attr;
  int err;

  err = pthread_condattr_init(&attr);
  if (err)
    return -err;

#if !defined(__ANDROID__)
  err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (err)
    goto error2;
#endif

  err = pthread_cond_init(cond, &attr);
  if (err)
    goto error2;

  err = pthread_condattr_destroy(&attr);
  if (err)
    goto error;

  return 0;

error:
  pthread_cond_destroy(cond);
error2:
  pthread_condattr_destroy(&attr);
  return -err;
}

#endif /* defined(__APPLE__) && defined(__MACH__) */

ucond_t* ucond_create(void) {
  ucond_t* cond = (ucond_t*)malloc(sizeof(ucond_t));
  if (cond == NULL) return NULL;
  if (ucond_init(cond)) {
    free(cond);
    return NULL;
  }
  return cond;
}

void ucond_destroy(ucond_t* cond) {
  int rc = pthread_cond_destroy(cond);
  free(cond);
  if (rc) abort();
}

void ucond_signal(ucond_t* cond) {
  if (pthread_cond_signal(cond))
    abort();
}

void ucond_broadcast(ucond_t* cond) {
  if (pthread_cond_broadcast(cond))
    abort();
}

void ucond_wait(ucond_t* cond, umutex_t* mutex) {
  if (pthread_cond_wait(cond, mutex))
    abort();
}

int ucond_timedwait(ucond_t* cond, umutex_t* mutex, uint64_t timeout) {
  int r;
  struct timespec ts;

#if defined(__APPLE__) && defined(__MACH__)
  ts.tv_sec = timeout / NANOSEC;
  ts.tv_nsec = timeout % NANOSEC;
  r = pthread_cond_timedwait_relative_np(cond, mutex, &ts);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  timeout += (tv.tv_sec * NANOSEC) + (tv.tv_usec * 1000);
  ts.tv_sec = timeout / NANOSEC;
  ts.tv_nsec = timeout % NANOSEC;
#if defined(__ANDROID__)
  r = pthread_cond_timedwait_monotonic_np(cond, mutex, &ts);
#else
  r = pthread_cond_timedwait(cond, mutex, &ts);
#endif /* __ANDROID__ */
#endif


  if (r == 0)
    return 0;

  if (r == ETIMEDOUT)
    return -ETIMEDOUT;

  abort();
  return -EINVAL;  /* Satisfy the compiler. */
}

#if defined(__APPLE__) && defined(__MACH__)

static int usem_init(usem_t* sem, unsigned int value) {
  kern_return_t err;

  err = semaphore_create(mach_task_self(), sem, SYNC_POLICY_FIFO, value);
  if (err == KERN_SUCCESS)
    return 0;
  if (err == KERN_INVALID_ARGUMENT)
    return -EINVAL;
  if (err == KERN_RESOURCE_SHORTAGE)
    return -ENOMEM;

  abort();
  return -EINVAL;  /* Satisfy the compiler. */
}

void usem_destroy(usem_t* sem) {
  int rc = semaphore_destroy(mach_task_self(), *sem);
  free(sem);
  if (rc) abort();
}

void usem_post(usem_t* sem) {
  if (semaphore_signal(*sem))
    abort();
}

void usem_wait(usem_t* sem) {
  int r;

  do
    r = semaphore_wait(*sem);
  while (r == KERN_ABORTED);

  if (r != KERN_SUCCESS)
    abort();
}

int usem_trywait(usem_t* sem) {
  mach_timespec_t interval;
  kern_return_t err;

  interval.tv_sec = 0;
  interval.tv_nsec = 0;

  err = semaphore_timedwait(*sem, interval);
  if (err == KERN_SUCCESS)
    return 0;
  if (err == KERN_OPERATION_TIMED_OUT)
    return -EAGAIN;

  abort();
  return -EINVAL;  /* Satisfy the compiler. */
}

#else /* !(defined(__APPLE__) && defined(__MACH__)) */

static int usem_init(usem_t* sem, unsigned int value) {
  if (sem_init(sem, 0, value))
    return -errno;
  return 0;
}

void usem_destroy(usem_t* sem) {
  int rc = sem_destroy(sem);
  free(sem);
  if (rc) abort();
}

void usem_post(usem_t* sem) {
  if (sem_post(sem))
    abort();
}

void usem_wait(usem_t* sem) {
  int r;

  do
    r = sem_wait(sem);
  while (r == -1 && errno == EINTR);

  if (r)
    abort();
}

int usem_trywait(usem_t* sem) {
  int r;

  do
    r = sem_trywait(sem);
  while (r == -1 && errno == EINTR);

  if (r) {
    if (errno == EAGAIN)
      return -EAGAIN;
    abort();
  }

  return 0;
}

#endif /* defined(__APPLE__) && defined(__MACH__) */

usem_t* usem_create(unsigned int value) {
  usem_t* sem;
  sem = malloc(sizeof(*sem));
  if (sem == NULL)
    return NULL;
  if (usem_init(sem, value)) {
    free(sem);
    return NULL;
  }
  return sem;
}

