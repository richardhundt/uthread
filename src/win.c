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
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include "uthread.h"

void ucond_destroy(ucond_t* cond);
void ucond_signal(ucond_t* cond);
void ucond_broadcast(ucond_t* cond);
void ucond_wait(ucond_t* cond, umutex_t* mutex);
int ucond_timedwait(ucond_t* cond, umutex_t* mutex, uint64_t timeout);

static UINT __stdcall uthread_start(void* arg) {
  struct uthread_ctx *ctx_p;
  struct uthread_ctx ctx;

  ctx_p = arg;
  ctx = *ctx_p;
  free(ctx_p);
  ctx.entry(ctx.arg);

  return 0;
}

int uthread_join(uthread_t *tid) {
  if (WaitForSingleObject(*tid, INFINITE))
    return GetLastError();
  else {
    CloseHandle(*tid);
    *tid = 0;
    return 0;
  }
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

  *tid = (HANDLE) _beginthreadex(NULL, 0, uthread_start, ctx, 0, NULL);
  err = *tid ? 0 : errno;

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
  return (unsigned long) GetCurrentThreadId();
}


static int umutex_init(umutex_t* mutex) {
  InitializeCriticalSection(mutex);
  return 0;
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
  DeleteCriticalSection(mutex);
  free(mutex);
}

void umutex_lock(umutex_t* mutex) {
  EnterCriticalSection(mutex);
}

int umutex_trylock(umutex_t* mutex) {
  if (TryEnterCriticalSection(mutex))
    return 0;
  else
    return EAGAIN;
}

void umutex_unlock(umutex_t* mutex) {
  LeaveCriticalSection(mutex);
}

static int ucond__init(ucond_t* cond) {
  int err;

  /* Initialize the count to 0. */
  cond->waiters_count = 0;

  InitializeCriticalSection(&cond->waiters_count_lock);

  /* Create an auto-reset event. */
  cond->signal_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!cond->signal_event) {
    err = GetLastError();
    goto error2;
  }

  /* Create a manual-reset event. */
  cond->broadcast_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!cond->broadcast_event) {
    err = GetLastError();
    goto error;
  }

  return 0;

error:
  CloseHandle(cond->signal_event);
error2:
  DeleteCriticalSection(&cond->waiters_count_lock);
  return err;
}

static int ucond_init(ucond_t* cond) {
  static int seen_init = 0;
  if (!seen_init) {
    seen_init = 1;
    SetErrorMode(
      SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
    );
  }

  return ucond__init(cond);
}

ucond_t* ucond_create(void) {
  ucond_t* cond = (ucond_t*)malloc(sizeof(ucond_t));
  if (ucond_init(cond)) {
    free(cond);
    return NULL;
  }
  return cond;
}

void ucond_destroy(ucond_t* cond) {
  if (!CloseHandle(cond->broadcast_event))
    abort();
  if (!CloseHandle(cond->signal_event))
    abort();
  DeleteCriticalSection(&cond->waiters_count_lock);
  free(cond);
}

void ucond_signal(ucond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->waiters_count_lock);
  have_waiters = cond->waiters_count > 0;
  LeaveCriticalSection(&cond->waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->signal_event);
}

void ucond_broadcast(ucond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->waiters_count_lock);
  have_waiters = cond->waiters_count > 0;
  LeaveCriticalSection(&cond->waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->broadcast_event);
}

inline int ucond_wait_helper(ucond_t* cond, umutex_t* mutex, DWORD dwMilliseconds) {
  DWORD result;
  int last_waiter;
  HANDLE handles[2] = {
    cond->signal_event,
    cond->broadcast_event
  };

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->waiters_count_lock);
  cond->waiters_count++;
  LeaveCriticalSection(&cond->waiters_count_lock);

  /* It's ok to release the <mutex> here since Win32 manual-reset events */
  /* maintain state when used with <SetEvent>. This avoids the "lost wakeup" */
  /* bug. */
  umutex_unlock(mutex);

  /* Wait for either event to become signaled due to <ucond_signal> being */
  /* called or <ucond_broadcast> being called. */
  result = WaitForMultipleObjects(2, handles, FALSE, dwMilliseconds);

  EnterCriticalSection(&cond->waiters_count_lock);
  cond->waiters_count--;
  last_waiter = result == WAIT_OBJECT_0 + 1
      && cond->waiters_count == 0;
  LeaveCriticalSection(&cond->waiters_count_lock);

  /* Some thread called <ucond_broadcast>. */
  if (last_waiter) {
    /* We're the last waiter to be notified or to stop waiting, so reset the */
    /* the manual-reset event. */
    ResetEvent(cond->broadcast_event);
  }

  /* Reacquire the <mutex>. */
  umutex_lock(mutex);

  if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1)
    return 0;

  if (result == WAIT_TIMEOUT)
    return WAIT_TIMEOUT;

  abort();
  return -1; /* Satisfy the compiler. */
}

void ucond_wait(ucond_t* cond, umutex_t* mutex) {
  if (ucond_wait_helper(cond, mutex, INFINITE))
    abort();
}

int ucond_timedwait(ucond_t* cond, umutex_t* mutex, uint64_t timeout) {
  return ucond_wait_helper(cond, mutex, (DWORD)(timeout / 1e6));
}


int usem_init(usem_t* sem, unsigned int value) {
  *sem = CreateSemaphore(NULL, value, INT_MAX, NULL);
  if (*sem == NULL)
    return -GetLastError();
  else
    return 0;
}

usem_t* usem_create(unsigned int value) {
  usem_t* sem;
  sem = malloc(sizeof(*sem));
  if (sem == NULL)
    return NULL;
  int rc = usem_init(sem, value);
  if (rc) {
    free(sem);
    return NULL;
  }
  return sem;
}

void usem_destroy(usem_t* sem) {
  int rc = CloseHandle(*sem);
  free(sem);
  if (!rc) abort()
}

void usem_post(usem_t* sem) {
  if (!ReleaseSemaphore(*sem, 1, NULL))
    abort();
}

void usem_wait(usem_t* sem) {
  if (WaitForSingleObject(*sem, INFINITE) != WAIT_OBJECT_0)
    abort();
}

int usem_trywait(usem_t* sem) {
  DWORD r = WaitForSingleObject(*sem, 0);

  if (r == WAIT_OBJECT_0)
    return 0;

  if (r == WAIT_TIMEOUT)
    return -EAGAIN;

  abort();
  return -1; /* Satisfy the compiler. */
}


