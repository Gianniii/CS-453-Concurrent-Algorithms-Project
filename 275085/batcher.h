#include <stdlib.h>

#include "lock.h"
#include "segment.h"
#include <tm.h>

/** shared memory region structure (1 per shared memory).
 * @param batcher Batcher instance for the shared memory
 * @param start Start of the shared memory region
 * @param segment Array of segments in the memory region
 * @param num_alloc_segments Number of allocated segments (used to keep track
 *for realloc)
 * @param first_seg_size Size of the shared memory region (in bytes)
 * @param align Claimed alignment of the shared memory region (in bytes)
 * @param align_alloc Actual alignment of the memory allocations (in bytes)
 * @param current_segment_index Max index of the current segment (incremented if
 *no freed indexes available)
 * @param freed_segment_index Array of indexes freed and that can be used again
 * @param segment_lock Lock for reallocation of array of segments and array of
 *freed indexes
 * @param curren_transaction_id Max value of transaction id assigned to some tx
 **/
typedef struct batcher_s {
  int cur_epoch;           // keep track of the current epoch through a counter
  int remaining;           // remaining threads in counter
  int blocked_count;       // number of blocked transacation threads
  lock_t lock;             // lock for batcher functions
  pthread_cond_t cond_var; // conditional variable for waking waiting threads
  int num_running_tx;      // current number of transacations running in batcher
  bool no_rw_tx; // ture if no rw ops in current epoch(for commit optimization)
  bool *is_ro;   // Array to keep track which transacations are read-only
} batcher_t;

typedef struct region_s {
  batcher_t batcher;
  void *start;
  // struct link allocs;
  segment_t *segment;
  int num_alloc_segments;
  size_t first_seg_size;
  size_t align;
  size_t align_alloc;
  int current_segment_index; // start from 1
  int *freed_segment_index;
  lock_t segment_lock;
  _Atomic(tx_t) current_transaction_id; // start from 1
} region_t;

bool init_batcher(batcher_t *);
int get_epoch(batcher_t *);
void enter(batcher_t *);
void leave(batcher_t *, region_t *, tx_t tx);
void destroy_batcher(batcher_t *);
