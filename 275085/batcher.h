#pragma once

#include "lock.h"
#include "segment.h"
#include "tm.h"
#include <stdlib.h>

typedef struct batcher_s {
  int cur_epoch;           // keep track of the current epoch through a counter
  int remaining;           // remaining threads in counter
  int blocked_count;       // number of blocked transacation threads
  lock_t lock;             // lock for batcher functions
  pthread_cond_t cond_var; // conditional variable for waking waiting threads
  int num_running_tx;      // current number of transacations running in batcher
  bool *is_ro; // Array to keep track which transacations are read-only
} batcher_t;

/* @param first_seg_size Size of the shared memory region (in bytes)
 * @param align Claimed alignment of the shared memory region (in bytes)
 * @param align_alloc Actual alignment of the memory allocations (in bytes)
 * @param current_segment_index Max index of the current segment (incremented if
 *no freed indexes available)
 * @param freed_segment_index Array of indexes freed and that can be used again
 * @param segment_lock Lock for reallocation of array of segments and array of
 *freed indexes
 * @param curren_transaction_id Max value of transaction id assigned to some tx
 **/
typedef struct region_s {
  _Atomic(tx_t) current_transaction_id; // start from 1
  void *start;                          // start of shared memory region
  segment_t *segment;                   // Array for its segments
  int num_alloc_segments; // TODO changethis num_alloc_segments Number of
                          // allocated segments (used to keep track
  //*for realloc)
  size_t align;
  size_t align_alloc;
  int *freed_segment_index;
  int current_segment_index; // start from 1
  lock_t segment_lock;
  batcher_t batcher;
  size_t first_seg_size;
} region_t;

bool init_batcher(batcher_t *);
bool enter_batcher(batcher_t *);
bool leave_batcher(region_t *, tx_t tx);
void destroy_batcher(batcher_t *);

void abort_tx(region_t *, tx_t);
void commit_tx(region_t *, tx_t);