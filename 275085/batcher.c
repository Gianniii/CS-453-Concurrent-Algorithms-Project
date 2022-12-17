#include "batcher.h"

bool init_batcher(batcher_t *batcher) {
  if (!lock_init(&(batcher->lock))) {
    return false;
  }
  batcher->tx_id_generator = 0; // id's start from 0
  batcher->n_blocked = 0;       // obv
  batcher->cur_epoch = 0;       // start at epoch 0
  batcher->n_in_epoch = 0;      // init num of tx in epoch is 0
  batcher->n_remaining = 0;     // initial num tx in batcher is 0
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

  // if last transaction is leaving
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
  batcher->n_remaining = batcher->n_blocked;
  batcher->n_in_epoch = batcher->n_blocked;
  batcher->tx_id_generator = 0;
  batcher->cur_epoch++;
  batcher->n_blocked = 0;
}

// Commit is only called once last transacation of epoch leaves the batcher
void commit_transcations_in_epoch(shared_t shared, tx_t unused(tx)) {
  region_t *region = (region_t *)shared;
  segment_t *segment;
  // go through all valid segments
  for (int i = 0; i < region->n_segments; i++) {
    segment = &region->segments[i];
    // free segments that were set to be freed by a transaction on this epoch
    if (segment->deregistered != NONE) {
      push(&(region->free_seg_indices), i);
    } else {
      // commit the written words of this segment and reset segment vals
      control_t *control = segment->control;
      for (size_t j = 0; j < segment->n_words; j++) {
        if (control[j].word_has_been_written == true) {
          control[j].word_is_ro = !control[j].word_is_ro;
        }
        // set metadata for next epoch
        control[j].word_has_been_written = false;
        control[j].access_set = NONE;
      }
    }
  }
}
