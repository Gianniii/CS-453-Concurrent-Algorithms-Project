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
typedef struct region_s
{
  _Atomic(tx_t) current_transaction_id;
  void *start;
  int num_alloc_segments;
  segment_t *segment;     // Array of segments
                          // allocated segments (used to keep track //maybe could use size of stack for this..
  //*for realloc)
  size_t align;
  int *freed_segment_index;  // array of freed segment indexes available fo reallocation, (-1 if not available)
  atomic_int num_existing_segments; // start from 1
  struct lock_t segment_lock;
  struct lock_t stack_lock; //for stack
  batcher_t batcher;
  size_t first_seg_size;
  stack_t free_seg_indices;
} region_t;

// we have a shared memory region between threads, what exactly re segments for? is a 1 segment per batch?
bool init_batcher(batcher_t *);
bool enter_batcher(batcher_t *);
bool leave_batcher(region_t *, tx_t tx);
void destroy_batcher(batcher_t *);
void prepare_batcher_for_next_epoch(batcher_t *batcher);

void abort_tx(region_t *, tx_t);
void commit_tx(region_t *, tx_t);
