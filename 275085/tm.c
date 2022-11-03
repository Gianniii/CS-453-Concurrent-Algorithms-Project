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
#include "tm.h"

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

  // calculate alignment for the shared memory region
  size_t align_alloc = align < sizeof(void *) ? sizeof(void *) : align;

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
      (int *)malloc(INIT_FREED_SEG_SIZE * sizeof(int));
  if (region->freed_segment_index == NULL) {
    destroy_batcher(&(region->batcher));
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  memset(region->freed_segment_index, -1, INIT_FREED_SEG_SIZE * sizeof(int));

  // init lock array of freed segments
  if (!lock_init(&(region->segment_lock))) {
    destroy_batcher(&(region->batcher));
    free(region->freed_segment_index);
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  // initialize first segment
  if (!segment_init(&region->segment[0], -1, size, align_alloc)) {
    destroy_batcher(&(region->batcher));
    lock_cleanup(&(region->segment_lock));
    free(region->freed_segment_index);
    free(region->segment);
    free(region);
    return invalid_shared;
  }

  region->start = encode_segment_address(0);

  region->first_seg_size = size;
  region->align = align;
  region->align_alloc = align_alloc;
  region->num_alloc_segments = INIT_SEG_SIZE;

  region->current_segment_index = 1;
  atomic_store(&region->current_transaction_id, (tx_t)1);

  return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy(shared_t shared) {
  region_t *region = (region_t *)shared;
  // cleanup and free batcher
  destroy_batcher(&(region->batcher));

  // free segment and related
  for (int i = 0; i < region->current_segment_index; i++) {
    segment_t seg = region->segment[i];
    free(seg.copy_0);
    free(seg.copy_1);
    free(seg.read_only_copy);
    free(seg.access_set);
    free(seg.is_written_in_epoch);
    free(seg.index_modified_words);
    for (int j = 0; j < (int)seg.num_words; j++) {
      lock_cleanup(&(seg.word_locks[j]));
    }
    free(seg.word_locks);
  }
  free(region->segment);
  free(region->freed_segment_index);

  // locks clean-up
  lock_cleanup(&(region->segment_lock));

  free(region);
}

void *tm_start(shared_t shared) { return ((region_t *)shared)->start; }

size_t tm_size(shared_t shared) { return ((region_t *)shared)->first_seg_size; }

size_t tm_align(shared_t shared) { return ((region_t *)shared)->align_alloc; }

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin(shared_t shared, bool is_ro) {
  // TODO LOCK with a tm_begin_lock or something
  region_t *region = (region_t *)shared;
  tx_t tx_index;

  // enter batcher
  if (!enter(&(region->batcher))) {
    return invalid_tx;
  }

  // create new tx element (get and add 1)
  // need to mod the index as it's always increasing
  tx_index = atomic_fetch_add(&region->current_transaction_id, 1) %
             region->batcher.num_running_tx;

  // save the kind of tx
  region->batcher.is_ro[tx_index] = is_ro;
  return tx_index;
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end(shared_t shared, tx_t tx) {
  region_t *region = (region_t *)shared;
  leave(&(region->batcher), region, tx);
  return true;
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
  segment_t *segment;
  int segment_index;
  int word_index;
  int num_words_to_read;
  alloc_t result;
  int offset;
  bool is_ro;

  // get transaction type
  is_ro = region->batcher.is_ro[tx];

  /**SANITY CHECKS**/

  // check size, must be multiple of the shared memory region’s alignment,
  // otherwise the behavior is undefined.
  if (size <= 0 || size % region->align_alloc != 0) {
    if (!is_ro) {
      abort_tx(region, tx);
    }
    return false; // abort_tx
  }

  // retrieve segment and word number
  decode_segment_address(source, &segment_index, &word_index);

  // check that source and target addresses are a positive multiple of the
  // shared memory region’s alignment, otherwise the behavior is undefined.
  if (word_index % region->align_alloc != 0 ||
      (uintptr_t)source % region->align_alloc != 0 ||
      segment_index > region->current_segment_index) {
    if (!is_ro) {
      abort_tx(region, tx);
    }
    return false; // abort_tx
  }

  // find true index (divide by word size)
  word_index = word_index / region->segment[segment_index].align;

  // check address correctness
  if (segment_index < 0 || word_index < 0) {
    if (!is_ro) {
      abort_tx(region, tx);
    }
    return false; // abort_tx
  }

  /**READ OPERATION**/

  // calculate number of words to be read in segment
  num_words_to_read = size / region->align_alloc;
  // get segment
  segment = &region->segment[segment_index];

  // loop through all word indexes (starting from the passed one)
  for (int curr_word_index = word_index;
       curr_word_index < word_index + num_words_to_read; curr_word_index++) {
    offset = (curr_word_index - word_index) * segment->align;
    result = read_word(curr_word_index, target + (offset), segment, is_ro, tx);
    if (result == abort_alloc) {
      abort_tx(region, tx);
      return false; // abort_tx
    }
  }

  // mark that segment has been modified only if read-write tx
  if (!is_ro && segment->has_been_modified == false) {
    segment->has_been_modified = true;
  }
  return true;
}

/** [thread-safe] Read word operation.
 * @param word_index Index of word into consideration
 * @param target Target start address (in a private region)
 * @param segment Pointer to the segment into consideration
 * @param is_ro Is read-only flag (about tx)
 * @param tx Current transaction identifier
 * @return Whether the whole transaction can continue (success/nomem), or not
 *(abort_alloc)
 **/
alloc_t read_word(int word_index, void *target, segment_t *segment, bool is_ro,
                  tx_t tx) {
  int readable_copy;
  int modified_index;
  // find readable copy
  readable_copy = segment->read_only_copy[word_index];

  // if read-only
  if (is_ro == true) {
    // perform read operation into target
    if (readable_copy == 0) {
      memcpy(target, segment->copy_0 + (word_index * segment->align),
             segment->align);
    } else {
      memcpy(target, segment->copy_1 + (word_index * segment->align),
             segment->align);
    }

    return success_alloc;
  } else {
    // if read-write

    // acquire word lock
    lock_acquire(&segment->word_locks[word_index]);

    // if word written in current epoch
    if (segment->is_written_in_epoch[word_index] == true) {

      // release word lock
      lock_release(&segment->word_locks[word_index]);

      // if transaction in access set
      if (segment->access_set[word_index] == tx) {
        // read write copy into target
        if (readable_copy == 0) {
          memcpy(target, segment->copy_1 + (word_index * segment->align),
                 segment->align);
        } else {
          memcpy(target, segment->copy_0 + (word_index * segment->align),
                 segment->align);
        }
        return success_alloc;
      } else {
        return abort_alloc;
      }
    } else {
      if (readable_copy == 0) {
        memcpy(target, segment->copy_0 + (word_index * segment->align),
               segment->align);
      } else {
        memcpy(target, segment->copy_1 + (word_index * segment->align),
               segment->align);
      }

      // mark that word has been read, after fetch&inc index, if:
      // - read-write tx
      // - not added before
      if (segment->access_set[word_index] == INVALID_TX) {
        modified_index =
            atomic_fetch_add(&segment->cnt_index_modified_words, 1);
        segment->index_modified_words[modified_index] = word_index;

        // add tx into access set (only the first tx)
        segment->access_set[word_index] = tx;
      }

      // release word lock
      lock_release(&segment->word_locks[word_index]);

      return success_alloc;
    }
  }
}

/** [thread-safe] Write operation in the given transaction, source in a private
 *region and target in the shared region.
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
  segment_t *segment;
  int segment_index;
  int word_index;
  int num_words_to_write;
  alloc_t result;
  int offset;

  /**SANITY CHECKS**/
  // check size, must be multiple of the shared memory region’s alignment,
  // otherwise the behavior is undefined.
  if (size <= 0 || size % region->align_alloc != 0) {
    abort_tx(region, tx);
    return false; // abort_tx
  }

  // retrieve segment and word number
  decode_segment_address(target, &segment_index, &word_index);

  // check that source and target addresses are a positive multiple of the
  // shared memory region’s alignment, otherwise the behavior is undefined.
  if (word_index % region->align_alloc != 0 ||
      (uintptr_t)target % region->align_alloc != 0) {
    abort_tx(region, tx);
    return false; // abort_tx
  }

  // calculate correct index (before it was * word_size)
  word_index = word_index / region->segment[segment_index].align;

  // check address correctness
  if (segment_index < 0 || word_index < 0) {
    abort_tx(region, tx);
    return false; // abort_tx
  }

  /**WRITE OPERATION**/

  // calculate number of words to be read in segment
  num_words_to_write = size / region->align_alloc;

  // get segment
  segment = &region->segment[segment_index];

  // loop thorugh all word indexes (starting from the passed one)
  for (int curr_word_index = word_index;
       curr_word_index < word_index + num_words_to_write; curr_word_index++) {
    offset = (curr_word_index - word_index) * segment->align;

    result = write_word(curr_word_index, source + (offset), segment, tx);
    if (result == abort_alloc) {
      abort_tx(region, tx);
      return false; // abort_tx
    }
  }

  // mark that segment has been modified
  if (segment->has_been_modified == false) {
    segment->has_been_modified = true;
  }
  return true;
}

/** [thread-safe] Write word operation.
 * @param word_index Index of word into consideration
 * @param source Source start address (in a private region)
 * @param segment Pointer to the segment into consideration
 * @param tx Current transaction identifier
 * @return Whether the whole transaction can continue (success/nomem), or not
 *(abort_alloc)
 **/
alloc_t write_word(int word_index, const void *source, segment_t *segment,
                   tx_t tx) {
  int readable_copy;
  int modified_index;
  readable_copy = segment->read_only_copy[word_index];

  // acquire word lock
  lock_acquire(&segment->word_locks[word_index]);

  // if word has been written before
  if (segment->is_written_in_epoch[word_index] == true) {
    // release word lock
    lock_release(&segment->word_locks[word_index]);

    // if tx in the access set
    if (segment->access_set[word_index] == tx) {
      // write source into write copy
      if (readable_copy == 0) {
        memcpy(segment->copy_1 + (word_index * segment->align), source,
               segment->align);
      } else {
        memcpy(segment->copy_0 + (word_index * segment->align), source,
               segment->align);
      }

      return success_alloc;
    } else {
      return abort_alloc;
    }
  } else {
    // if one other tx in access set
    if (segment->access_set[word_index] != tx &&
        segment->access_set[word_index] != INVALID_TX) {
      lock_release(&segment->word_locks[word_index]);
      return abort_alloc;
    } else {
      // write source into write copy
      if (readable_copy == 0) {
        memcpy(segment->copy_1 + (word_index * segment->align), source,
               segment->align);
      } else {
        memcpy(segment->copy_0 + (word_index * segment->align), source,
               segment->align);
      }

      // mark that word has been written, after fetch&inc index, if:
      // - not added before
      if (segment->access_set[word_index] == INVALID_TX) {
        modified_index =
            atomic_fetch_add(&segment->cnt_index_modified_words, 1);
        segment->index_modified_words[modified_index] = word_index;

        // add tx into access set (only the first tx)
        segment->access_set[word_index] = tx;
      }

      // mark word as it has been written
      segment->is_written_in_epoch[word_index] = true;

      lock_release(&segment->word_locks[word_index]);

      return success_alloc;
    }
  }
}

/** [thread-safe] Memory allocation in the given transaction. (should be called
 *a lot)
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
  segment_t segment;
  int index = -1;
  // check correct alignment of size
  if (size <= 0 || size % region->align_alloc != 0) {
    abort_tx(region, tx);
    return abort_alloc; // abort_tx
  }

  // check if there is a shared index for segment
  lock_acquire(&(region->segment_lock));
  for (int i = 0; i < region->current_segment_index; i++) {
    if (region->freed_segment_index[i] != -1) {
      index = i;
      break;
    }
  }
  // if no index found in freed, calculate new one
  if (index == -1) {
    // fetch and increment
    index = region->current_segment_index;
    region->current_segment_index++;

    // acquire realloc lock
    if (index >= region->num_alloc_segments) {
      region->segment = (segment_t *)realloc(
          region->segment, sizeof(segment_t) * 2 * region->num_alloc_segments);
      region->freed_segment_index =
          (int *)realloc(region->freed_segment_index,
                         sizeof(int) * 2 * region->num_alloc_segments);
      if (region->segment == NULL || region->freed_segment_index == NULL) {
        abort_tx(region, tx);
        return nomem_alloc;
      }

      // update number of allocated segments
      region->num_alloc_segments *= 2;
    }

    // init segment
    if (!segment_init(&segment, tx, size, region->align_alloc)) {
      return nomem_alloc;
    }

    // insert segment into segment array and set freed to occupied
    region->segment[index] = segment;

  } else {
    // init segment
    if (!segment_init(&region->segment[index], tx, size, region->align_alloc)) {
      return nomem_alloc;
    }
  }

  // mark that segment has been modified
  region->segment[index].has_been_modified = true;

  // update support array freed_segment_index
  region->freed_segment_index[index] = -1;
  lock_release(&(region->segment_lock));

  // return encoded address to segment
  *target = encode_segment_address(index);
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
  int segment_index;
  int word_index;
  unsigned long int cmp_val = INVALID_TX;
  region_t *region = (region_t *)shared;

  // retrieve segment and word number
  decode_segment_address(target, &segment_index, &word_index);

  // check address correctness (can't free 1st segment or an address which is
  // not pointing to the 1st word)
  if (segment_index == 0 || word_index != 0) {
    abort_tx(region, tx);
    return false; // abort_tx
  }

  // free (set to tx to_delete) segment from array of segments
  atomic_compare_exchange_strong(&region->segment[segment_index].to_delete,
                                 &cmp_val,
                                 tx); // 0 should be a pointer to a value

  // abort_tx concurrent transactions (or sequentials) which called free on
  // segment after some other transaction
  if (region->segment[segment_index].to_delete != tx) {
    abort_tx(region, tx);
    return false; // abort_tx
  }
  // mark that segment has been modified
  if (region->segment[segment_index].has_been_modified == false) {
    region->segment[segment_index].has_been_modified = true;
  }
  return true;
}

/** [thread-safe] abort_tx operations for a given transaction
 * @param region Shared memory region
 * @param tx Current transaction
 **/
void abort_tx(region_t *region, tx_t tx) {
  unsigned long int invalid_value = INVALID_TX;
  int max_segment_index;
  int max_word_index;
  segment_t *segment;
  tx_t tx_tmp;
  int word_index;

  // store current max segment index
  lock_acquire(&(region->segment_lock));
  max_segment_index = region->current_segment_index;
  lock_release(&(region->segment_lock));

  // for all segments
  for (int segment_index = 0;
       segment_index < max_segment_index &&
       region->freed_segment_index[segment_index] == -1 &&
       region->segment[segment_index].has_been_modified == true;
       segment_index++) {
    segment = &region->segment[segment_index];

    // unset segments on which tx has called tm_free previously (tx_tmp is
    // needed because of the atomic_compare_exchange, which perform if then else
    // (check code at the bottom))
    tx_tmp = tx;
    atomic_compare_exchange_strong(
        &segment->to_delete, &tx,
        invalid_value); // tx should be a pointer to a value
    tx = tx_tmp;

    // add used segment indexes for tx to freed_segment_index array (for
    // tm_alloc)
    if (segment->created_by_tx == tx) {
      region->freed_segment_index[segment_index] = segment_index;
      segment->created_by_tx = INVALID_TX;
      continue;
    }

    max_word_index = atomic_load(&segment->cnt_index_modified_words);

    // roolback write operations on each word performed by tx
    for (int i = 0; i < max_word_index; i++) {
      word_index = segment->index_modified_words[i];
      if (segment->access_set[word_index] == tx &&
          segment->is_written_in_epoch[word_index] == true) {
        lock_acquire(&segment->word_locks[word_index]);
        segment->access_set[word_index] = INVALID_TX;
        segment->is_written_in_epoch[word_index] = false;
        lock_release(&segment->word_locks[word_index]);
      }
    }
  }
  // also aborting tx should leave the batcher
  leave(&(region->batcher), region, tx);
}

/** [thread-safe] commit operations for a given transaction
 * @param region Shared memory region
 * @param tx Current transaction
 **/
void commit_tx(region_t *region, tx_t unused(tx)) {
  segment_t *segment;
  int word_index;

  for (int segment_index = 0;
       segment_index < region->current_segment_index &&
       region->freed_segment_index[segment_index] == -1 &&
       region->segment[segment_index].has_been_modified == true;
       segment_index++) {
    segment = &region->segment[segment_index];

    // add to freed_segment_index array segments which have been freed by tx
    if (segment->to_delete != INVALID_TX) {
      region->freed_segment_index[segment_index] = segment_index; // so freed
      continue;
    }
    // add used segment indexes for tx to freed_segment_index array
    if (segment->created_by_tx != INVALID_TX) {
      segment->created_by_tx = INVALID_TX;
    }
    // commit algorithm for words (copy written word copy into read-only copy)
    for (int i = 0; i < (int)segment->cnt_index_modified_words; i++) {
      word_index = segment->index_modified_words[i];
      if (segment->is_written_in_epoch[word_index] == true) {
        // swap valid copy (in case it has been written)
        segment->read_only_copy[word_index] =
            (segment->read_only_copy[word_index] == 0 ? 1 : 0);
        // empty access set
        segment->access_set[word_index] = INVALID_TX;
        // empty is_written_in_epoch flag
        segment->is_written_in_epoch[word_index] = false;

      } else if (segment->access_set[word_index] != INVALID_TX) {
        // empty access set
        segment->access_set[word_index] = INVALID_TX;
      }
    }
    // reset flags
    segment->has_been_modified = false;
    memset(segment->index_modified_words, -1,
           segment->cnt_index_modified_words * sizeof(int));
    segment->cnt_index_modified_words = 0;
  }
}
