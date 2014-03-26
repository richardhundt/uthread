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

#define HAVE_SRWLOCK_API() (pTryAcquireSRWLockShared != NULL)
#define HAVE_CONDVAR_API() (pInitializeConditionVariable != NULL)

inline static int ucond_fallback_init(ucond_t* cond);
inline static void ucond_fallback_destroy(ucond_t* cond);
inline static void ucond_fallback_signal(ucond_t* cond);
inline static void ucond_fallback_broadcast(ucond_t* cond);
inline static void ucond_fallback_wait(ucond_t* cond, umutex_t* mutex);
inline static int ucond_fallback_timedwait(ucond_t* cond,
    umutex_t* mutex, uint64_t timeout);

inline static int ucond_condvar_init(ucond_t* cond);
inline static void ucond_condvar_destroy(ucond_t* cond);
inline static void ucond_condvar_signal(ucond_t* cond);
inline static void ucond_condvar_broadcast(ucond_t* cond);
inline static void ucond_condvar_wait(ucond_t* cond, umutex_t* mutex);
inline static int ucond_condvar_timedwait(ucond_t* cond,
    umutex_t* mutex, uint64_t timeout);

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
    return utranslate_sys_error(GetLastError());
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
  if (ctx == NULL) return NULL;

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


int umutex_init(umutex_t* mutex) {
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

inline static int ucond_fallback_init(ucond_t* cond) {
  int err;

  /* Initialize the count to 0. */
  cond->fallback.waiters_count = 0;

  InitializeCriticalSection(&cond->fallback.waiters_count_lock);

  /* Create an auto-reset event. */
  cond->fallback.signal_event = CreateEvent(NULL,  /* no security */
                                            FALSE, /* auto-reset event */
                                            FALSE, /* non-signaled initially */
                                            NULL); /* unnamed */
  if (!cond->fallback.signal_event) {
    err = GetLastError();
    goto error2;
  }

  /* Create a manual-reset event. */
  cond->fallback.broadcast_event = CreateEvent(NULL,  /* no security */
                                               TRUE,  /* manual-reset */
                                               FALSE, /* non-signaled */
                                               NULL); /* unnamed */
  if (!cond->fallback.broadcast_event) {
    err = GetLastError();
    goto error;
  }

  return 0;

error:
  CloseHandle(cond->fallback.signal_event);
error2:
  DeleteCriticalSection(&cond->fallback.waiters_count_lock);
  return err;
}


inline static int ucond_condvar_init(ucond_t* cond) {
  pInitializeConditionVariable(&cond->cond_var);
  return 0;
}

int ucond_init(ucond_t* cond) {
  static int seen_init = 0;
  if (!seen_init) {
    seen_init = 1;
    SetErrorMode(
      SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
    );
  }

  if (HAVE_CONDVAR_API())
    return ucond_condvar_init(cond);
  else
    return ucond_fallback_init(cond);
}

ucond_t* ucond_create(void) {
  ucond_t* cond = (ucond_t*)malloc(sizeof(ucond_t));
  if (ucond_init(cond)) {
    free(cond);
    return NULL;
  }
  return cond;
}

inline static void ucond_fallback_destroy(ucond_t* cond) {
  if (!CloseHandle(cond->fallback.broadcast_event))
    abort();
  if (!CloseHandle(cond->fallback.signal_event))
    abort();
  DeleteCriticalSection(&cond->fallback.waiters_count_lock);
}


inline static void ucond_condvar_destroy(ucond_t* cond) {
  /* nothing to do */
}


void ucond_destroy(ucond_t* cond) {
  if (HAVE_CONDVAR_API())
    ucond_condvar_destroy(cond);
  else
    ucond_fallback_destroy(cond);
  free(cond);
}


inline static void ucond_fallback_signal(ucond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  have_waiters = cond->fallback.waiters_count > 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->fallback.signal_event);
}


inline static void ucond_condvar_signal(ucond_t* cond) {
  pWakeConditionVariable(&cond->cond_var);
}


void ucond_signal(ucond_t* cond) {
  if (HAVE_CONDVAR_API())
    ucond_condvar_signal(cond);
  else
    ucond_fallback_signal(cond);
}


inline static void ucond_fallback_broadcast(ucond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  have_waiters = cond->fallback.waiters_count > 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->fallback.broadcast_event);
}


inline static void ucond_condvar_broadcast(ucond_t* cond) {
  pWakeAllConditionVariable(&cond->cond_var);
}


void ucond_broadcast(ucond_t* cond) {
  if (HAVE_CONDVAR_API())
    ucond_condvar_broadcast(cond);
  else
    ucond_fallback_broadcast(cond);
}


inline int ucond_wait_helper(ucond_t* cond, umutex_t* mutex,
    DWORD dwMilliseconds) {
  DWORD result;
  int last_waiter;
  HANDLE handles[2] = {
    cond->fallback.signal_event,
    cond->fallback.broadcast_event
  };

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  cond->fallback.waiters_count++;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  /* It's ok to release the <mutex> here since Win32 manual-reset events */
  /* maintain state when used with <SetEvent>. This avoids the "lost wakeup" */
  /* bug. */
  umutex_unlock(mutex);

  /* Wait for either event to become signaled due to <ucond_signal> being */
  /* called or <ucond_broadcast> being called. */
  result = WaitForMultipleObjects(2, handles, FALSE, dwMilliseconds);

  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  cond->fallback.waiters_count--;
  last_waiter = result == WAIT_OBJECT_0 + 1
      && cond->fallback.waiters_count == 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  /* Some thread called <ucond_broadcast>. */
  if (last_waiter) {
    /* We're the last waiter to be notified or to stop waiting, so reset the */
    /* the manual-reset event. */
    ResetEvent(cond->fallback.broadcast_event);
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


inline static void ucond_fallback_wait(ucond_t* cond, umutex_t* mutex) {
  if (ucond_wait_helper(cond, mutex, INFINITE))
    abort();
}

inline static void ucond_condvar_wait(ucond_t* cond, umutex_t* mutex) {
  if (!pSleepConditionVariableCS(&cond->cond_var, mutex, INFINITE))
    abort();
}


void ucond_wait(ucond_t* cond, umutex_t* mutex) {
  if (HAVE_CONDVAR_API())
    ucond_condvar_wait(cond, mutex);
  else
    ucond_fallback_wait(cond, mutex);
}


inline static int ucond_fallback_timedwait(ucond_t* cond,
    umutex_t* mutex, uint64_t timeout) {
  return ucond_wait_helper(cond, mutex, (DWORD)(timeout / 1e6));
}


inline static int ucond_condvar_timedwait(ucond_t* cond,
    umutex_t* mutex, uint64_t timeout) {
  if (pSleepConditionVariableCS(&cond->cond_var, mutex, (DWORD)(timeout / 1e6)))
    return 0;
  if (GetLastError() != ERROR_TIMEOUT)
    abort();
  return ERROR_TIMEOUT;
}


int ucond_timedwait(ucond_t* cond, umutex_t* mutex,
    uint64_t timeout) {
  if (HAVE_CONDVAR_API())
    return ucond_condvar_timedwait(cond, mutex, timeout);
  else
    return ucond_fallback_timedwait(cond, mutex, timeout);
}

