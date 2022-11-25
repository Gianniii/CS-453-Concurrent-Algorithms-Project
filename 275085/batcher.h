#pragma once

#include "lock.h"
#include "segment.h"
#include "stack.h"
#include "tm.h"
#include <stdlib.h>
// Region and batcher(i.e segment management structs)
typedef struct {
  int tx_id_generator;
  int cur_epoch; // From description: keep track of the current epoch through a
                 // counter
  int n_remaining;    // From project description: remaining threads in counter
  int n_blocked;      // number of blocked transacation threads
  int n_in_epoch;     // current number of transacations running in batcher
  struct lock_t lock; // lock for batcher functions also has a
                      // conditional variable for waking waiting threads,
                      // Like batcher from description has to do
} batcher_t;

typedef struct region_s {
  void *start; // start of shared memory region
  atomic_int n_segments;
  segment_t *segments;   // Array of segments
  struct lock_t global_lock; //only used partly tm_free/alloc and abort
  // struct lock_t stack_lock; //for stack
  batcher_t batcher;
  size_t align;
  size_t seg_size; // just for tm_size
  bool is_ro_flags[MAX_NUM_SEGMENTS];  // Array to keep track which transacations are read-only,
                    // by mapping their id <->index in array
  stack_t free_seg_indices; //store indices of freed segment for reuse
} region_t;

bool init_batcher(batcher_t *batcher);
tx_t enter_batcher(batcher_t *batcher);
bool leave_batcher(shared_t shared, tx_t tx);
void prepare_batcher_for_next_epoch(batcher_t *batcher);
void commit_transcations_in_epoch(shared_t shared, tx_t tx);

bool abort_transaction_tx(shared_t shared, tx_t tx);
