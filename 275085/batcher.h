#pragma once

#include "lock.h"
#include "tm.h"
#include <stdlib.h>
#include "stack.h"
#include "segment.h"
//Region and batcher(i.e segment management structs)
typedef struct
{
  int cur_epoch;           // From description: keep track of the current epoch through a counter
  int n_remaining;         // From project description: remaining threads in counter
  int n_blocked;           // number of blocked transacation threads
  int n_in_epoch;          // current number of transacations running in batcher
  struct lock_t lock;      // lock for batcher functions
  pthread_cond_t all_tx_left; // conditional variable for waking waiting threads(Like batcer from description)
  bool *is_ro;             // Array to keep track which transacations are read-only, by mapping their id <->index in array
} batcher_t;

typedef struct region_s
{
  atomic_int tx_counter; 
  void *start; //start of shared memory region
  int num_alloc_segments;
  segment_t *segment; // Array of segments
                      // allocated segments (used to keep track //maybe could use size of stack for this..
  //*for realloc)
  bool *segment_is_free;         // array of freed segment indexes available fo reallocation, (-1 if not available)
  atomic_int num_existing_segments; // start from 1
  struct lock_t global_lock;
  // struct lock_t stack_lock; //for stack
  batcher_t batcher;
  size_t align;
  size_t seg_size; //just for tm_size
  // stack_t free_seg_indices;
} region_t;


// we have a shared memory region between threads, what exactly re segments for? is a 1 segment per batch?
bool init_batcher(batcher_t * batcher);
void destroy_batcher(batcher_t * batcher);
bool enter_batcher(batcher_t * batcher);
bool leave_batcher(shared_t shared, tx_t tx);
void prepare_batcher_for_next_epoch(batcher_t *batcher);

bool abort_transaction_tx(shared_t shared, tx_t tx);
void commit_transcations_in_epoch(shared_t  shared, tx_t tx);
