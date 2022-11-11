/**
 * @file   tm.c
 * @author Gianni Lodetti
 **/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif
// External headers
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/** Internal headers **/
#include "batcher.h"
#include "lock.h"
#include "macros.h"
#include "segment.h"
#include "stack.h"
#include "tm.h"

#define NOT_FREE -1

/** Create (i.e. allocate + init) a new shared memory region, with one first
 *non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in
 *bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared
 *memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create(size_t size, size_t align) {
  // allocate shared memory region
  region_t *region = (region_t *)malloc(sizeof(region_t));
  if (!region) {
    return invalid_shared;
  }
  // initialize batcher for shared memory region
  if (!init_batcher(&region->batcher)) {
    free(region);
    return invalid_shared;
  }

  // allocate and initialize 1st segment in shared memory region
  region->segment = (segment_t *)malloc(INIT_SEG_SIZE * sizeof(segment_t));
  if (!region->segment) {
    destroy_batcher(&(region->batcher));
    free(region);
    return invalid_shared;
  }

  // allocate freed segment array (to track freed segment indexes)
  region->freed_segment_index =
      (int *)malloc(MAX_NUM_SEGMENTS * sizeof(int));
  if (region->freed_segment_index == NULL) {
    destroy_batcher(&(region->batcher));
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  memset(region->freed_segment_index, NOT_FREE, MAX_NUM_SEGMENTS * sizeof(int));

  // init lock array of freed segments
  if (!lock_init(&(region->segment_lock))) {
    destroy_batcher(&(region->batcher));
    free(region->freed_segment_index);
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  // calculate alignment for the shared memory region
  align = align < sizeof(void *) ? sizeof(void *) : align;
  // initialize first segment
  if (!segment_init(&region->segment[0], -1, size, align)) {
    destroy_batcher(&(region->batcher));
    lock_cleanup(&(region->segment_lock));
    free(region->freed_segment_index);
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  region->start = get_virt_addr(0);

  region->seg_size = size;
  region->align = align;
  region->num_alloc_segments = INIT_SEG_SIZE;

  region->num_existing_segments = 1;
  region->current_transaction_id = 1;

  return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy(shared_t shared) {
  region_t *region = (region_t *)shared;

  // free segment and related
  for (int i = 0; i < region->num_existing_segments; i++) {
    segment_t seg = region->segment[i];
    free(seg.cp0);
    free(seg.cp1);
    free(seg.cp_is_ro);
    free(seg.access_set);
    free(seg.is_written_in_epoch);
    for (size_t i = 0; i < seg.n_words; i++) {
      lock_cleanup(&(seg.word_locks[i]));
    }
    free(seg.word_locks);
  }

  destroy_batcher(&(region->batcher));
  free(region->segment);
  free(region->freed_segment_index);

  // locks clean-up
  lock_cleanup(&(region->segment_lock));

  free(region);
}

void *tm_start(shared_t shared) { return ((region_t *)shared)->start; }

size_t tm_size(shared_t shared) { return ((region_t *)shared)->seg_size; }

size_t tm_align(shared_t shared) { return ((region_t *)shared)->align; }

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin(shared_t shared, bool is_ro) {
  // TODO LOCK with a tm_begin_lock or something
  region_t *region = (region_t *)shared;
  // enter batcher
  if (!enter_batcher(&(region->batcher))) {
    return invalid_tx;
  }
  // get index of transacation
  tx_t id = atomic_fetch_add(&region->current_transaction_id, 1) %
            region->batcher.n_in_epoch;
  region->batcher.is_ro[id] = is_ro; //no need for atomic since this is unique to each transcation
  return id;
}

bool tm_end(shared_t shared, tx_t tx) {
  return leave_batcher(
      (region_t *)shared,
      tx); // will never need to return false because we we notify when abort
}

/** [thread-safe] Read operation in the given transaction, source in the shared
 *region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the
 *alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
 **/
bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size,
             void *target) {
  region_t *region = (region_t *)shared;

  // get necessary data
  bool is_ro = region->batcher.is_ro[tx];
  int n_words = size / region->align;
  int segment_index = extract_seg_id_from_virt_addr(source);
  int word_index = extract_word_index_from_virt_addr(
      source, region->segment[segment_index].align);
  segment_t *segment = &region->segment[segment_index];

  // Read words, if fail to read a word then abort
  int curr_word_offset = 0;
  int read_idx = word_index;
  while (read_idx < word_index + n_words) {
    if (read_word(read_idx, target + (curr_word_offset * segment->align),
                  segment, is_ro, tx) == abort_alloc) {
      ;
      abort_tx(region, tx);
      return false;
    }
    read_idx++;
    curr_word_offset++;
  }
  return true;
}

// UTILS
// ===================================================================================
void add_to_access_set(int word_index, segment_t *seg, tx_t tx) {
  if (seg->access_set[word_index] == INVALID_TX) {
    // here could push index of modified words
    seg->access_set[word_index] = tx;
  }
}

void read_correct_copy(int word_index, void *target, segment_t *seg, int copy) {
  void *start_words_addr = copy == 0 ? seg->cp0 : seg->cp1;
  memcpy(target, start_words_addr + (word_index * seg->align), seg->align);
}

void write_to_correct_copy(int word_index, const void *src, segment_t *seg,
                           int copy, tx_t tx) {
  seg->is_written_in_epoch[word_index] = true; // set is_written flag to true;
  void *start_words_addr = copy == 0 ? seg->cp1 : seg->cp0;
  memcpy(start_words_addr + (word_index * seg->align), src, seg->align);
  // if word has not been written in before, update supporting datastructures
  if (seg->access_set[word_index] == INVALID_TX) {
    add_to_access_set(word_index, seg, tx);
  }
}

bool allocate_more_segments(region_t *region) {
  // if index is beyond number of allocated segments then allocated another one
  if (region->num_existing_segments >= region->num_alloc_segments) {
    region->segment = (segment_t *)realloc(
        region->segment, sizeof(segment_t) * region->num_alloc_segments + 1);
    if (region->segment == NULL) { // check realloc is successfull
      return false;
    }
    // update number of allocated segments
    region->num_alloc_segments += 1;
  }
  return true;
}

//Care: Segment must be locked during this operation
int add_segment(region_t *region) {
  int idx = region->num_existing_segments;
  region->num_existing_segments++;
  // might have to allocate more segments to accomodate this extra segment
  if (!allocate_more_segments(region)) {
    return -1;
  }
  // add segment structure to region
  // TODO Make region contains list of segment pointers! so can allocated all
  // pointers at start but will have to remember to free all the pointers
  // within... so maybe not cool
  segment_t segment;
  region->segment[idx] = segment; // copy segment
  // printf("goes here\n");
  return idx;
}

//=========================================================================================
// Like in project description, slightly simplified the if/else to something i
// understood better but outcome is the same
alloc_t read_word(int word_index, void *target, segment_t *segment, bool is_ro,
                  tx_t tx) {

  int ro_copy = segment->cp_is_ro[word_index];
  int write_copy = (ro_copy == 0) ? 1 : 0;
  if (is_ro == true) {
    read_correct_copy(word_index, target, segment, ro_copy);
    return success_alloc;
  } else { // r_w transacation
    lock_acquire(&segment->word_locks[word_index]);
    // if word not written, can read the r_o copy
    if (segment->is_written_in_epoch[word_index] == false) {
      // if first access and not read only transaction
      if (segment->access_set[word_index] == INVALID_TX) {
        add_to_access_set(word_index, segment, tx);
      }
      lock_release(
          &segment
               ->word_locks[word_index]); // allow parallel reads on same word
      read_correct_copy(word_index, target, segment, ro_copy);
      return success_alloc;
    }

    // if word written in current epoch by "this" transcation then can read,
    // else abort
    if (segment->is_written_in_epoch[word_index] == true &&
        segment->access_set[word_index] == tx) {
      lock_release(
          &segment
               ->word_locks[word_index]); // allow parallel reads on same word
      read_correct_copy(word_index, target, segment, write_copy);
      return success_alloc;
    } else {
      lock_release(&segment->word_locks[word_index]);
      return abort_alloc;
    }
  }
}

/** [thread-safe] Write operation in the given transaction, source in a
 *private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the
 *alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
 **/
bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size,
              void *target) {
  region_t *region = (region_t *)shared;
  int segment_index = extract_seg_id_from_virt_addr(target);
  segment_t *segment = &region->segment[segment_index];
  int word_index = extract_word_index_from_virt_addr(
      target, region->segment[segment_index].align);
  int n_words = size / region->align;
  int curr_word_offset = 0;
  int write_idx = word_index;

  while (write_idx < word_index + n_words) {
    if (write_word(write_idx, source + curr_word_offset, segment, tx) ==
        abort_alloc) {
      abort_tx(region, tx);
      return false;
    }
    write_idx++;
    curr_word_offset++;
  }
  return true;
}

// Like in project description
alloc_t write_word(int word_index, const void *source, segment_t *segment,
                   tx_t tx) {
  int ro_copy = segment->cp_is_ro[word_index];

  // acquire word lock
  lock_acquire(&segment->word_locks[word_index]);

  // if word has been written before
  if (segment->is_written_in_epoch[word_index] == true) {
    // release word lock
    lock_release(&segment->word_locks[word_index]);

    // if tx in the access set
    if (segment->access_set[word_index] == tx) {
      write_to_correct_copy(word_index, source, segment, ro_copy, tx);
      return success_alloc;
    } else {
      return abort_alloc;
    }
  } else { // word has not been written before
    // if one other tx in access set
    if (segment->access_set[word_index] != tx &&
        segment->access_set[word_index] != INVALID_TX) {
      lock_release(&segment->word_locks[word_index]);
      return abort_alloc;
    } else {
      write_to_correct_copy(word_index, source, segment, ro_copy, tx);
      lock_release(&segment->word_locks[word_index]);
      return success_alloc;
    }
  }
}

/** [thread-safe] Memory allocation in the given transaction. (should be
 *called a lot)  (allocated a new segment!)
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive
 *multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first
 *byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not
 *(abort_alloc)
 **/
alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void **target) {
  region_t *region = (region_t *)shared;

  // check if there is a shared index for segment
  // printf("%d", region->num_existing_segments);
  lock_acquire(&(region->segment_lock));
  int i = 0;
  int index = -1;
  // check if can reuse one of the already allocated segments
  while (index == -1 && i < region->num_existing_segments) {
    if (region->freed_segment_index[i] != NOT_FREE) {
      index = i;
    }
    i++;
  }
  if (index == -1) { // cannot reuse already allocated segment
    index = add_segment(region);
    if (index == -1) {
      return nomem_alloc;
    }
  }

  // intialize new segment with calculated index
  if (!segment_init(&region->segment[index], tx, size, region->align)) {
    return nomem_alloc;
  }
  // no longer free index
  region->freed_segment_index[index] = NOT_FREE;

  // set target to virtual address of the segment
  *target = get_virt_addr(index);
  lock_release(&(region->segment_lock));
  return success_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment
 *to deallocate
 * @return Whether the whole transaction can continue
 **/
bool tm_free(shared_t shared, tx_t tx, void *target) {
  region_t *region = (region_t*)shared;

  int segment_index = extract_seg_id_from_virt_addr(target);
  // set to be deregistered like written in project description
  lock_acquire(&(region->segment_lock)); //TODO more finegrain locking or atomic variables in segment
      if(region->segment[segment_index].deregistered == INVALID_TX) { //if segment is not deregisterd then deregister it
        region->segment[segment_index].deregistered = tx;
      } else {
        lock_release(&(region->segment_lock));
        //if has already been deregistered then abort the transacation
        if (region->segment[segment_index].deregistered != tx) {
          abort_tx(region, tx);
          return false; // abort_tx
        }
      }
  lock_release(&(region->segment_lock));
  return true;
}

/** [thread-safe] abort_tx operations for a given transaction
 * @param region Shared memory region
 * @param tx Current transaction
 **/
void abort_tx(region_t *region, tx_t tx) {

  //Iterate over all non free'd segments
  int max_segment_index = region->num_existing_segments;
  segment_t *segment;
  for (int segment_index = 0; segment_index < max_segment_index;
       segment_index++) {
    if( region->freed_segment_index[segment_index] == NOT_FREE) {

    
      segment = &region->segment[segment_index];

      // unset segments on which tx has called tm_free previously (tx_tmp is
      // needed because of the atomic_compare_exchange, which perform if then
      // else (check code at the bottom))
      lock_acquire(&(region->segment_lock)); //TODO more finegrain locking or atomic variables in segment
        if(segment->deregistered == tx) { //re-register the segment
          segment->deregistered = INVALID_TX;
        } 
      lock_release(&(region->segment_lock));

      //free the segment that was created for "this" aborted transaction
      if (segment->tx_id_of_creator == tx) {
        region->freed_segment_index[segment_index] = segment_index;
      } else {
        //Check words this tx accessed and wrote to and and make those words available again
        for (size_t i = 0; i < segment->n_words;
            i++) { // TODO: Can optimize by using stack of modified word_indexes
                    // instead of iterating over all words and checking if they have
                    // been written
          if (segment->access_set[i] == tx &&
              segment->is_written_in_epoch[i] == true) {
            lock_acquire(&segment->word_locks[i]);
            segment->access_set[i] = INVALID_TX;
            segment->is_written_in_epoch[i] = false;
            lock_release(&segment->word_locks[i]);
          }
        }
      }
    }
  }
  // also aborting tx should leave the batcher
  leave_batcher(region, tx);
}

/** [thread-safe] commit operations for a given transaction
 * @param region Shared memory region
 * @param tx Current transaction
 **/
//Commit is only called once last transacation of epoch leaves the batcher
void commit_tx(region_t *region, tx_t unused(tx)) {
  segment_t *segment;
  // go through all valid segments(not freed)
  for (int segment_index = 0; segment_index < region->num_alloc_segments;
       segment_index++) {
    if(region->freed_segment_index[segment_index] == NOT_FREE) {
      segment = &region->segment[segment_index];
      // free segments that were set to be freed by a transaction on this epoch
      if (segment->deregistered != INVALID_TX) {
        region->freed_segment_index[segment_index] = segment_index; 
      } else {
        //commit the written words of this segment and reset segment vals
        for (size_t i = 0; i < segment->n_words; i++) {
          if (segment->is_written_in_epoch[i] == true) {
            segment->cp_is_ro[i] = (segment->cp_is_ro[i]+1)%2;
        }
        //set metadata for next epoch
        segment->is_written_in_epoch[i] = false;
        segment->access_set[i] = INVALID_TX;
        segment->tx_id_of_creator = INVALID_TX; //creator no longer exists
        }
      }
    }
  }
}
