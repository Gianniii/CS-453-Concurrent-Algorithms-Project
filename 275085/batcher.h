#pragma once

#include "lock.h"
#include "segment.h"
#include "tm.h"
#include <stdlib.h>
#include "stack.h"

typedef struct batcher_s
{
  int cur_epoch;           // keep track of the current epoch through a counter
  int n_remaining;         // remaining threads in counter
  int n_blocked;           // number of blocked transacation threads
  int n_in_epoch;          // current number of transacations running in batcher
  struct lock_t lock;      // lock for batcher functions
  pthread_cond_t cond_var; // conditional variable for waking waiting threads
  bool *is_ro;             // Array to keep track which transacations are read-only
} batcher_t;

typedef struct region_s
{
  _Atomic(tx_t) current_transaction_id;
  void *start;
  int num_alloc_segments;
  segment_t *segment; // Array of segments
                      // allocated segments (used to keep track //maybe could use size of stack for this..
  //*for realloc)
  size_t align;
  int *segment_is_free;         // array of freed segment indexes available fo reallocation, (-1 if not available)
  atomic_int num_existing_segments; // start from 1
  struct lock_t segment_lock;
  // struct lock_t stack_lock; //for stack
  batcher_t batcher;
  size_t seg_size;
  // stack_t free_seg_indices;
} region_t;

// we have a shared memory region between threads, what exactly re segments for? is a 1 segment per batch?
bool init_batcher(batcher_t * batcher);
bool enter_batcher(batcher_t * batcher);
bool leave_batcher(region_t * region, tx_t tx);
void destroy_batcher(batcher_t * batcher);
void prepare_batcher_for_next_epoch(batcher_t *batcher);

bool abort_transaction_tx(region_t * region, tx_t tx);
void commit_transacations_in_epoch(region_t * region, tx_t tx);
