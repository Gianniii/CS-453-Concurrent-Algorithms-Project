#include "lock.h"

bool lock_init(struct lock_t *lock) {
  return pthread_mutex_init(&(lock->mutex), NULL) == 0 &&
         pthread_cond_init(&(lock->all_tx_left_batcher), NULL) == 0;
}

void lock_cleanup(struct lock_t *lock) {
  pthread_mutex_destroy(&(lock->mutex));
  pthread_cond_destroy(&(lock->all_tx_left_batcher));
}

bool lock_acquire(struct lock_t *lock) {
  return pthread_mutex_lock(&(lock->mutex)) == 0;
}

void lock_release(struct lock_t *lock) { pthread_mutex_unlock(&(lock->mutex)); }

void lock_wait(struct lock_t *lock) {
  pthread_cond_wait(&(lock->all_tx_left_batcher), &(lock->mutex));
}

void lock_wake_up(struct lock_t *lock) {
  pthread_cond_broadcast(&(lock->all_tx_left_batcher));
}
