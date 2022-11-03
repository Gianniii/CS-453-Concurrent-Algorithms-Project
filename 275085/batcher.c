#include "batcher.h"

bool init_batcher(batcher_t *batcher) {
  batcher->blocked_count = 0;
  batcher->cur_epoch = 0;
  batcher->num_running_tx = 0;
  batcher->remaining = 0;
  batcher->is_ro = NULL;
  if (!lock_init(&(batcher->lock))) {
    return false;
  }
  if (pthread_cond_init(&(batcher->cond_var), NULL) != 0) {
    lock_cleanup(&(batcher->lock));
    return false;
  }
  return true;
}

// Enters in the critical section, or waits until woken up.
bool enter_batcher(batcher_t *batcher) {
  lock_acquire(&batcher->lock);
  // if firs tx to enter batcher
  if (batcher->remaining == 0) {
    batcher->remaining = 1;
    batcher->num_running_tx = batcher->remaining;
    // alloc is_ro for this first tx.
    batcher->is_ro = (bool *)malloc(sizeof(bool));
    if (batcher->is_ro == NULL) {
      return false;
    }
  } else {
    // block and wait for next epoch.
    batcher->blocked_count++;
    pthread_cond_wait(&batcher->cond_var, &batcher->lock.mutex);
  }
  lock_release(&batcher->lock);
  return true;
}

// Leave and wake up other threads if are last
bool leave_batcher(region_t *region, tx_t tx) {
  batcher_t *batcher = &(region->batcher);
  lock_acquire(&batcher->lock);

  batcher->remaining--;

  // Last transaction is leaving
  if (batcher->remaining == 0) {
    batcher->cur_epoch++;
    // prepare batcher to unblock waiting transacations -------
    batcher->remaining = batcher->blocked_count;
    // realloc transactions array with new number of transactions
    if (batcher->remaining == 0) {
      free(batcher->is_ro); // free because will allocating again once another
                            // enters
      batcher->is_ro = NULL;
    } else {
      batcher->is_ro =
          (bool *)realloc(batcher->is_ro, batcher->remaining * sizeof(bool));
    }
    batcher->num_running_tx = batcher->remaining;
    commit_tx(region, tx); // commit all transacations

    batcher->blocked_count = 0;
    // Unblock waiting transactions
    pthread_cond_broadcast(&batcher->cond_var);
  }
  lock_release(&batcher->lock);
  return true;
}

void destroy_batcher(batcher_t *batcher) {
  lock_cleanup(&(batcher->lock));
  pthread_cond_destroy(&(batcher->cond_var));
}