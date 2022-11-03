#include "lock.h"

bool lock_init(lock_t *lock) {
  return pthread_mutex_init(&(lock->mutex), NULL) == 0;
}
void lock_cleanup(lock_t *lock) { pthread_mutex_destroy(&(lock->mutex)); }
bool lock_acquire(lock_t *lock) {
  return pthread_mutex_lock(&(lock->mutex)) == 0;
}
void lock_release(lock_t *lock) { pthread_mutex_unlock(&(lock->mutex)); }