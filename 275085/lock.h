#pragma once

#include <pthread.h>
#include <stdbool.h>

/** Locks
 * @param mutex mutex
 **/
typedef struct lock_s {
  pthread_mutex_t mutex;
} lock_t;

// additional functions
bool lock_init(lock_t *);
void lock_cleanup(lock_t *);
bool lock_acquire(lock_t *);
void lock_release(lock_t *);
