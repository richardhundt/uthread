#ifndef _UTHREAD_H_
#define _UTHREAD_H_

#if defined(_WIN) || defined(_WIN32)

#ifndef _WIN32_WINNT
# define _WIN32_WINNT   0x0502
#endif

#if !defined(_SSIZE_T_) && !defined(_SSIZE_T_DEFINED)
typedef intptr_t ssize_t;
# define _SSIZE_T_
# define _SSIZE_T_DEFINED
#endif

#include <windows.h>
#include <process.h>
#include <stdint.h>

typedef HANDLE uthread_t;
typedef CRITICAL_SECTION umutex_t;

typedef union {
  CONDITION_VARIABLE cond_var;
  struct {
    unsigned int waiters_count;
    CRITICAL_SECTION waiters_count_lock;
    HANDLE signal_event;
    HANDLE broadcast_event;
  } fallback;
} ucond_t;
#else
#include <stdint.h>
#include <pthread.h>
typedef pthread_t uthread_t;
typedef pthread_mutex_t umutex_t;
typedef pthread_cond_t ucond_t;
#endif

struct uthread_ctx {
  void (*entry)(void* arg);
  void* arg;
};

umutex_t* umutex_create(void);
int umutex_init(umutex_t* handle);
void umutex_destroy(umutex_t* handle);
void umutex_lock(umutex_t* handle);
int umutex_trylock(umutex_t* handle);
void umutex_unlock(umutex_t* handle);

ucond_t* ucond_create(void);
int ucond_init(ucond_t* cond);
void ucond_destroy(ucond_t* cond);
void ucond_signal(ucond_t* cond);
void ucond_broadcast(ucond_t* cond);
void ucond_wait(ucond_t* cond, umutex_t* mutex);
int ucond_timedwait(ucond_t* cond, umutex_t* mutex, uint64_t timeout);

uthread_t* uthread_create(void (*entry)(void *arg), void *arg);
void uthread_destroy(uthread_t* tid);
unsigned long uthread_self(void);
int uthread_join(uthread_t *tid);

#endif /* _UTHREAD_H_ */

