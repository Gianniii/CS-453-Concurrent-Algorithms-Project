#include "batcher.h"

bool init_batcher(batcher_t *batcher) {
  batcher->cur_epoch = 0;
  batcher->no_rw_tx = true;
  batcher->is_ro = NULL;
  batcher->remaining = 0;
  batcher->num_running_tx = 0;
  batcher->blocked_count = 0;
  if (!lock_init(&(batcher->lock))) {
    return false;
  }
  if (pthread_cond_init(&(batcher->cond_var), NULL) != 0) {
    lock_cleanup(&(batcher->lock));
    return false;
  }
  return true;
}

int get_epoch(batcher_t *batcher) { return batcher->cur_epoch; }

// Enters in the critical section, or waits until woken up.
void enter(batcher_t *batcher) {
  lock_acquire(&batcher->lock);
  if (batcher->remaining == 0) {
    // here only the first transaction of the STM enters
    batcher->remaining = 1;
    batcher->num_running_tx = batcher->remaining;

    // as it's the first, we need to allocate the array of running_tx in batcher
    batcher->is_ro = (bool *)malloc(sizeof(bool));
  } else {
    // If batcher has remaining, add to num_blocked_threads and wait
    batcher->blocked_count++;
    pthread_cond_wait(&batcher->cond_var, &batcher->lock.mutex);
  }
  lock_release(&batcher->lock);
  return;
}

// Leave and wake up other threads if are last
void leave(batcher_t *batcher, region_t *region, tx_t tx) {
  lock_acquire(&batcher->lock);

  // update number remaining transactions
  batcher->remaining--;

  // only the last tx leaving the batcher performs operations
  if (batcher->remaining == 0) {
    batcher->cur_epoch++;
    batcher->remaining = batcher->blocked_count;

    // realloc transactions array with new number of transactions
    if (batcher->remaining == 0) {
      free(batcher->is_ro);
    } else {
      batcher->is_ro =
          (bool *)realloc(batcher->is_ro, batcher->remaining * sizeof(bool));
    }
    batcher->num_running_tx = batcher->remaining;
    commit_tx(region, tx); // commit all transacations

    batcher->blocked_count = 0;
    batcher->no_rw_tx = true;

    pthread_cond_broadcast(&batcher->cond_var);
  }
  lock_release(&batcher->lock);
  return;
}

void destroy_batcher(batcher_t *batcher) {
  lock_cleanup(&(batcher->lock));
  pthread_cond_destroy(&(batcher->cond_var));
}