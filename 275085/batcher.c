#include "batcher.h"

bool init_batcher(batcher_t *batcher) {
  if (!lock_init(&(batcher->lock))) {
    return false;
  }
  if (pthread_cond_init(&(batcher->cond_var), NULL) != 0) {
    lock_cleanup(&(batcher->lock));
    return false;
  }
  batcher->n_blocked = 0;
  batcher->cur_epoch = 0;
  batcher->n_in_epoch = 0;
  batcher->n_remaining = 0;
  batcher->is_ro = malloc(sizeof(bool));
  if (batcher->is_ro == NULL) {
    return false;
  }
  return true;
}

// look project description
bool enter_batcher(batcher_t *batcher) {
  lock_acquire(&batcher->lock);
  // if firs tx to enter batcher
  if (batcher->n_remaining == 0) {
    batcher->n_remaining = 1;
    batcher->n_in_epoch = batcher->n_remaining;
    // alloc is_ro for this first tx.
  } else {
    // block and wait for next epoch.
    batcher->n_blocked++;
    pthread_cond_wait(&batcher->cond_var, &batcher->lock.mutex);
  }
  lock_release(&batcher->lock);
  return true;
}

// Leave and wake up other threads if are last
bool leave_batcher(region_t *region, tx_t tx) {
  batcher_t *batcher = &(region->batcher);
  lock_acquire(&batcher->lock);
  batcher->n_remaining--;

  // Last transaction is leaving
  if (batcher->n_remaining == 0) {
    commit_tx(region, tx);
    prepare_batcher_for_next_epoch(batcher);
    pthread_cond_broadcast(&batcher->cond_var);
  }
  lock_release(&batcher->lock);
  return true;
}

void destroy_batcher(batcher_t *batcher) {
  lock_cleanup(&(batcher->lock));
  if (batcher->is_ro != NULL)
    free(batcher->is_ro);
  pthread_cond_destroy(&(batcher->cond_var));
}

void prepare_batcher_for_next_epoch(batcher_t *batcher) {
  // prepare batcher to unblock waiting transacations -------
  batcher->n_remaining = batcher->n_blocked;
  batcher->n_in_epoch = batcher->n_blocked;
  // reallocate transactions array
  int size_to_alloc = batcher->n_blocked == 0 ? 1 : batcher->n_blocked;
  batcher->is_ro = realloc(batcher->is_ro, sizeof(bool) * size_to_alloc);

  batcher->cur_epoch++;
  batcher->n_blocked = 0;
}