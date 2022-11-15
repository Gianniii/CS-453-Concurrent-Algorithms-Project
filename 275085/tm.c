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

#define N_INIT_SEGMENTS 128 // To avoid constant reallocation
// init a shared memory region with one segment
shared_t tm_create(size_t size, size_t align) {
  // allocate shared memory region
  region_t *region = (region_t *)malloc(sizeof(region_t));
  if (!region) {
    return invalid_shared;
  }

  region->start = get_virt_addr(0);
  region->seg_size = size;
  region->align = align;
  region->n_segments = 1;
  region->tx_counter = 1;
  region->segment = (segment_t *)malloc(N_INIT_SEGMENTS * sizeof(segment_t));
  if (!region->segment) {
    free(region);
    return invalid_shared;
  }

  region->segment_is_free =
      (bool *)calloc(MAX_NUM_SEGMENTS, sizeof(bool)); // auto init all to false
  if (region->segment_is_free == NULL) {
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  if (!lock_init(&(region->global_lock))) {
    free(region->segment_is_free);
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  align = align < sizeof(void *) ? sizeof(void *) : align;
  if (!segment_init(&region->segment[0], size, align)) {
    lock_cleanup(&(region->global_lock));
    free(region->segment_is_free);
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  if (!init_batcher(&region->batcher)) {
    lock_cleanup(&(region->global_lock));
    free(region->segment_is_free);
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy(shared_t shared) {
  region_t *region = (region_t *)shared;

  // free segment and related
  for (int i = 0; i < region->n_segments; i++) {
    segment_t seg = region->segment[i];
    free(seg.words_array_A);
    free(seg.words_array_B);
    free(seg.word_is_ro);
    free(seg.access_set);
    free(seg.word_has_been_written_flag);
    for (size_t i = 0; i < seg.n_words; i++) {
      lock_cleanup(&(seg.word_lock[i]));
    }
    free(seg.word_lock);
  }

  // free batcher
  batcher_t batcher = region->batcher;
  pthread_cond_destroy(&(batcher.lock.all_tx_left_batcher));
  lock_cleanup(&(batcher.lock));
  if (batcher.is_ro_flags != NULL)
    free(batcher.is_ro_flags);
  free(region->segment);
  free(region->segment_is_free);

  lock_cleanup(&(region->global_lock));

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
  tx_t id = (tx_t)((atomic_fetch_add(&region->tx_counter, 1)) %
                   region->batcher.n_in_epoch);
  region->batcher.is_ro_flags[id] =
      is_ro; // no need for atomic since this is unique to each transcation
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

  // get necessary data for the loop
  int n_words = size / region->align;
  int segment_index = extract_seg_id_from_virt_addr(source);
  int source_start_index = extract_word_index_from_virt_addr(
      source, region->segment[segment_index].align);
  segment_t *seg = &region->segment[segment_index];

  // like in project description
  void *target_word_addr =
      target; // target location to read word(like in description)
  int source_word_idx = source_start_index;
  while (source_word_idx < source_start_index + n_words) {
    if (read_word(seg, tx, region->batcher.is_ro_flags[tx], source_word_idx,
                  target_word_addr) == abort_alloc) {
      return (abort_transaction_tx(region, tx));
    }
    source_word_idx++;
    target_word_addr += seg->align;
  }
  return true;
}

// UTILS
// ===================================================================================
void read_read_only_copy(int word_index, void *target, segment_t *seg) {
  void *start_words_addr = seg->word_is_ro[word_index] == 0
                               ? seg->words_array_A
                               : seg->words_array_B;
  memcpy(target, start_words_addr + (word_index * seg->align), seg->align);
}

void read_writable_copy(int word_index, void *target, segment_t *seg) {
  void *start_words_addr = (seg->word_is_ro[word_index] == 0)
                               ? seg->words_array_B
                               : seg->words_array_A;
  memcpy(target, start_words_addr + (word_index * seg->align), seg->align);
}

// write to copy that is not the read copy
void write_to_correct_copy(int word_index, const void *src, segment_t *seg) {
  void *start_words_addr = (seg->word_is_ro[word_index] == 0)
                               ? seg->words_array_B
                               : seg->words_array_A;
  memcpy(start_words_addr + (word_index * seg->align), src, seg->align);
}

bool allocate_more_segments(region_t *region) {
  // if index is beyond number of allocated segments then allocated another one
  if (region->n_segments >= N_INIT_SEGMENTS) {
    region->segment = (segment_t *)realloc(
        region->segment, sizeof(segment_t) * region->n_segments);
    if (region->segment == NULL) { // check realloc is successfull
      return false;
    }
  }
  return true;
}

// Care: Segment must be locked during this operation
int add_segment(region_t *region) {
  int idx = region->n_segments;
  region->n_segments++;
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
alloc_t read_word(segment_t *segment, tx_t tx, bool is_ro, int index,
                  void *target) {
  if (is_ro == true) {
    read_read_only_copy(index, target, segment);
    return success_alloc;
  } else { // r_w transacation
    lock_acquire(&segment->word_lock[index]);
    // if word not written, can read the r_o copy
    if (segment->word_has_been_written_flag[index] == false) {
      // if first access add myself to access_set
      if (segment->access_set[index] == NONE) {
        segment->access_set[index] = tx;
      }
      lock_release(
          &segment->word_lock[index]); // allow parallel reads on same word
      read_read_only_copy(index, target, segment);
      return success_alloc;
    }

    // if word written in current epoch by "this" transcation then can read,
    // else abort
    if (segment->word_has_been_written_flag[index] == true &&
        segment->access_set[index] == tx) {
      lock_release(
          &segment->word_lock[index]); // allow parallel reads on same word
      // read the writable copy
      read_writable_copy(index, target, segment);
      return success_alloc;
    } else {
      lock_release(&segment->word_lock[index]);
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
  int start_target_word_index = extract_word_index_from_virt_addr(
      target, region->segment[segment_index].align);
  int n_words = size / region->align;
  const void *word_source = (void *)source;
  int target_idx = start_target_word_index;
  while (target_idx < start_target_word_index + n_words) {

    if (write_word(segment, tx, target_idx, word_source) == abort_alloc) {
      return (abort_transaction_tx(region, tx));
    }
    target_idx++;
    word_source++;
  }
  return true;
}

// Exactly like Project description
alloc_t write_word(segment_t *segment, tx_t tx, int index, const void *source) {
  // acquire word lock
  lock_acquire(&segment->word_lock[index]);

  // if word has been written before
  if (segment->word_has_been_written_flag[index] == true) {
    // release word lock to allow concurrent write
    lock_release(&segment->word_lock[index]);

    // if tx in the access set
    if (segment->access_set[index] == tx) {
      write_to_correct_copy(index, source, segment);
      return success_alloc;
    } else {
      return abort_alloc;
    }
  } else { // abort if word has already been accessed by another tx
    // if one other tx in access set
    if (segment->access_set[index] != NONE &&
        segment->access_set[index] != tx) {
      lock_release(&segment->word_lock[index]);
      return abort_alloc;
    } else { // CASE: first to access and write to word. So update
             // datastructures and release lock to allow concurent writes
      segment->access_set[index] = tx;
      segment->word_has_been_written_flag[index] =
          true;                                 // set is_written flag to true;
      lock_release(&segment->word_lock[index]); // allow concurrent writes

      write_to_correct_copy(index, source, segment);
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
alloc_t tm_alloc(shared_t shared, tx_t unused(tx), size_t size, void **target) {
  region_t *region = (region_t *)shared;

  // check if there is a shared index for segment
  // printf("%d", region->n_segments);
  lock_acquire(&(region->global_lock));
  int i = 0;
  int index = -1;
  // check if can reuse one of the already allocated segments
  while (index == -1 && i < region->n_segments) {
    if (region->segment_is_free[i] != NOT_FREE) {
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
  if (!segment_init(&region->segment[index], size, region->align)) {
    return nomem_alloc;
  }
  // no longer free index
  region->segment_is_free[index] = NOT_FREE;

  // set target to virtual address of the segment
  *target = get_virt_addr(index);
  lock_release(&(region->global_lock));
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
  region_t *region = (region_t *)shared;

  int segment_index = extract_seg_id_from_virt_addr(target);
  // set to be deregistered like written in project description
  lock_acquire(&(region->global_lock)); // TODO more finegrain locking or atomic
                                        // variables in segment
  if (region->segment[segment_index].deregistered ==
      NONE) { // if segment is not deregisterd then deregister it
    region->segment[segment_index].deregistered = tx;
  } else {
    lock_release(&(region->global_lock));
    // if has already been deregistered then abort the transacation
    if (region->segment[segment_index].deregistered != tx) {
      return (abort_transaction_tx(region, tx));
    }
  }
  lock_release(&(region->global_lock));
  return true;
}

// always returns false
bool abort_transaction_tx(shared_t shared, tx_t tx) {
  region_t *region = (region_t *)shared;
  // Iterate over all non free'd segments
  int max_segment_index = region->n_segments;
  segment_t *segment;
  for (int segment_index = 0; segment_index < max_segment_index;
       segment_index++) {
    if (region->segment_is_free[segment_index] == NOT_FREE) {
      segment = &region->segment[segment_index];

      // unset segments on which tx has called tm_free previously (tx_tmp is
      // needed because of the atomic_compare_exchange, which perform if then
      // else (check code at the bottom))
      lock_acquire(&(region->global_lock)); // TODO more finegrain locking or
                                            // atomic variables in segment
      if (segment->deregistered == tx) {    // re-register the segment
        segment->deregistered = NONE;
      }
      lock_release(&(region->global_lock));
      // Check words this tx accessed and wrote to and and make those words
      // available again
      for (size_t i = 0; i < segment->n_words;
           i++) { // TODO: Can optimize by using stack of modified
                  // word_indexes instead of iterating over all words and
                  // checking if they have been written
        if (segment->access_set[i] == tx &&
            segment->word_has_been_written_flag[i] == true) {
          lock_acquire(&segment->word_lock[i]);
          segment->access_set[i] = NONE;
          segment->word_has_been_written_flag[i] = false;
          lock_release(&segment->word_lock[i]);
        }
      }
    }
  }
  leave_batcher(region, tx);
  return false;
}

/** [thread-safe] commit operations for a given transaction
 * @param region Shared memory region
 * @param tx Current transaction
 **/
// Commit is only called once last transacation of epoch leaves the batcher
void commit_transcations_in_epoch(shared_t shared, tx_t unused(tx)) {
  region_t *region = (region_t *)shared;
  segment_t *segment;
  // go through all valid segments(not freed)
  for (int segment_index = 0; segment_index < region->n_segments;
       segment_index++) {
    if (region->segment_is_free[segment_index] == NOT_FREE) {
      segment = &region->segment[segment_index];
      // free segments that were set to be freed by a transaction on this epoch
      if (segment->deregistered != NONE) {
        region->segment_is_free[segment_index] = FREE;
      } else {
        // commit the written words of this segment and reset segment vals
        for (size_t i = 0; i < segment->n_words; i++) {
          if (segment->word_has_been_written_flag[i] == true) {
            segment->word_is_ro[i] = (segment->word_is_ro[i] + 1) % 2;
          }
          // set metadata for next epoch
          segment->word_has_been_written_flag[i] = false;
          segment->access_set[i] = NONE;
        }
      }
    }
  }
}
