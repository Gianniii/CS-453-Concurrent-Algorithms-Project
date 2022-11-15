#include "batcher.h"

bool init_batcher(batcher_t *batcher) {
  if (!lock_init(&(batcher->lock))) {
    return false;
  }
  if (pthread_cond_init(&(batcher->lock.all_tx_left_batcher), NULL) != 0) {
    lock_cleanup(&(batcher->lock));
    return false;
  }
  batcher->tx_id_generator = 0;
  batcher->n_blocked = 0;   // obv
  batcher->cur_epoch = 0;   // start at epoch 0
  batcher->n_in_epoch = 0;  // init num of tx in epoch is 0
  batcher->n_remaining = 0; // initial num tx in batcher is 0
  batcher->is_ro_flags = malloc(sizeof(bool));
  if (batcher->is_ro_flags == NULL) {
    return false;
  }
  return true;
}

// Behavior from project description
tx_t enter_batcher(batcher_t *batcher) {
  lock_acquire(&batcher->lock);
  // if firs tx to enter batcher
  if (batcher->n_remaining == 0) {
    batcher->n_in_epoch = 1;
    batcher->n_remaining = 1;
  } else {
    // block and wait for next epoch.
    batcher->n_blocked++;
    pthread_cond_wait(&batcher->lock.all_tx_left_batcher, &batcher->lock.mutex);
  }
  tx_t id = batcher->tx_id_generator;
  batcher->tx_id_generator++;
  lock_release(&batcher->lock);
  return id;
}

// Leave and wake up other threads if are last
// Like in project description
bool leave_batcher(shared_t shared, tx_t tx) {
  region_t *region = (region_t *)shared;
  batcher_t *batcher = &(region->batcher);
  lock_acquire(&batcher->lock);

  // Last transaction is leaving
  if (batcher->n_remaining == 1) {
    commit_transcations_in_epoch(region, tx);
    prepare_batcher_for_next_epoch(batcher);
    pthread_cond_broadcast(&batcher->lock.all_tx_left_batcher);
  } else {
    batcher->n_remaining -= 1;
  }
  lock_release(&batcher->lock);
  return true;
}

void prepare_batcher_for_next_epoch(batcher_t *batcher) {
  // prepare batcher to unblock waiting transacations -------
  batcher->n_remaining = batcher->n_blocked;
  batcher->n_in_epoch = batcher->n_blocked;
  batcher->tx_id_generator = 0;

  int size_to_alloc = batcher->n_blocked == 0 ? 1 : batcher->n_blocked;
  batcher->is_ro_flags =
      realloc(batcher->is_ro_flags, sizeof(bool) * size_to_alloc);

  batcher->cur_epoch++;
  batcher->n_blocked = 0; // reset
}