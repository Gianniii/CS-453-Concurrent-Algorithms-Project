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
// CODE FOR VIRTUAL ADDRESS
// TRANSLATION-------------------------------------------------------------------
#define VIRT_ADDR_OFFSET                                                       \
  (intptr_t)(0x8000000) // offset for base of virtual
                        // addr
#define WORD_ADDR_SPACE_BITS 16;
#define LSB16 (intptr_t)0xFFFF

// VIRT_ADDR format: first 16 lsb are used for word address, the remaning bits
// are used to store segment id with
// an offset(because addr 0 not allowed) get virtual address from a segment id
#define GET_VIRT_ADDR(seg_id)                                                  \
  ((void *)((intptr_t)((VIRT_ADDR_OFFSET | seg_id) << 16)));
// remove offset and get 16 msb to get seg_id
#define EXTRACT_SEG_ID_FROM_VIRT_ADDR(addr)                                    \
  ((int)(VIRT_ADDR_OFFSET ^ (intptr_t)((intptr_t)addr >> 16)));
#define EXTRACT_WORD_INDEX_FROM_VIRT_ADDR(addr, align)                         \
  (((intptr_t)addr & LSB16) / align); // return id of word

//---------------------------------------------------------------------------------------------------------------
#define read_read_only_copy(word_index, target, seg)                           \
  {                                                                            \
    void *start_words_addr = seg->control[word_index].word_is_ro == false      \
                                 ? seg->words_array_A                          \
                                 : seg->words_array_B;                         \
    memcpy(target, start_words_addr + (word_index * seg->align), seg->align);  \
  }

#define read_writable_copy(word_index, target, seg)                            \
  {                                                                            \
    void *start_words_addr = (seg->control[word_index].word_is_ro == false)    \
                                 ? seg->words_array_B                          \
                                 : seg->words_array_A;                         \
    memcpy(target, start_words_addr + (word_index * seg->align), seg->align);  \
  }

// write to copy that is not the read copy
#define write_to_correct_copy(word_index, src, seg)                            \
  {                                                                            \
    void *start_words_addr = (seg->control[word_index].word_is_ro == false)    \
                                 ? seg->words_array_B                          \
                                 : seg->words_array_A;                         \
    memcpy(start_words_addr + (word_index * seg->align), src, seg->align);     \
  }

// init a shared memory region with one segment(here has more but doesnt matter
// its an optimization)
shared_t tm_create(size_t size, size_t align) {
  region_t *region = (region_t *)malloc(sizeof(region_t));
  if (!region) {
    return invalid_shared;
  }
  region->free_seg_indices.top = -1;
  region->addr = GET_VIRT_ADDR(0);
  region->seg_size = size;
  region->align = align;
  region->n_segments = 1;
  region->segments = (segment_t *)malloc(N_INIT_SEGMENTS * sizeof(segment_t));
  if (!region->segments) {
    free(region);
    return invalid_shared;
  }

  if (!lock_init(&(region->global_lock))) {
    free(region->segments);
    free(region);
    return invalid_shared;
  }

  if (!init_batcher(&region->batcher) |
      !init_segment(&region->segments[0], align, size)) {
    lock_cleanup(&(region->global_lock));
    free(region->segments);
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
  for (int i = 0; i < region->n_segments; i++) {
    segment_destroy(&(region->segments[i]));
  }
  lock_cleanup(&(region->batcher.lock));
  lock_cleanup(&(region->global_lock));
  free(region->segments);
  free(region);
}

void *tm_start(shared_t shared) { return ((region_t *)shared)->addr; }

size_t tm_size(shared_t shared) { return ((region_t *)shared)->seg_size; }

size_t tm_align(shared_t shared) { return ((region_t *)shared)->align; }

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin(shared_t shared, bool is_ro) {
  tx_t id = enter_batcher(&(((region_t *)shared)->batcher));
  ((region_t *)shared)->is_ro_flags[id] = is_ro;
  return id;
}

bool tm_end(shared_t shared, tx_t tx) {
  return leave_batcher((region_t *)shared, tx);
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
  int seg_id = EXTRACT_SEG_ID_FROM_VIRT_ADDR(source);
  segment_t *seg = &region->segments[seg_id];
  int source_start_index =
      EXTRACT_WORD_INDEX_FROM_VIRT_ADDR(source, seg->align);

  // like in project description
  void *target_word_addr =
      target; // target location to read word(like in description)
  int source_word_idx = source_start_index;
  while (source_word_idx < source_start_index + n_words) {
    if (read_word(seg, tx, region->is_ro_flags[tx], source_word_idx,
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
bool allocate_more_segments(region_t *region) {
  // if index is beyond number of allocated segments then allocated another one
  if (region->n_segments >= N_INIT_SEGMENTS) {
    region->segments = (segment_t *)realloc(
        region->segments, sizeof(segment_t) * region->n_segments);
    if (region->segments == NULL) {
      return false;
    }
  }
  return true;
}

// Care: Segment must be locked during this operation
int add_segment(region_t *region, size_t size) {
  int idx = region->n_segments;
  region->n_segments++;
  // might have to allocate more segments to accomodate this extra segment
  if (!allocate_more_segments(region)) {
    return -1;
  }
  // intialize new segment with calculated index
  if (!init_segment(&region->segments[idx], region->align, size)) {
    return nomem_alloc;
  }
  // printf("goes here\n");
  return idx;
}

//=========================================================================================

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
  int seg_id = EXTRACT_SEG_ID_FROM_VIRT_ADDR(target);
  segment_t *segment = &region->segments[seg_id];
  int start_target_word_index =
      EXTRACT_WORD_INDEX_FROM_VIRT_ADDR(target, region->segments[seg_id].align);
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

  // printf("%d", region->n_segments);
  lock_acquire(&(region->global_lock));
  // find a free index or allocate new one
  int segment_index = pop(&(region->free_seg_indices));
  if (segment_index == -1) { // cannot reuse already allocated segment
    segment_index = add_segment(region, size);
    if (segment_index == -1) {
      return nomem_alloc;
    }
  } else {
    // reset segment for re-use
    if (!init_segment(&region->segments[segment_index], region->align, size)) {
      return nomem_alloc;
    }
  }
  // set target to virtual address of the segment
  *target = GET_VIRT_ADDR(segment_index);
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

  int segment_index = EXTRACT_SEG_ID_FROM_VIRT_ADDR(target);
  // can only free deregistered seg's else abort
  // set to be deregistered like written in project description
  lock_acquire(&(region->global_lock)); // TODO more finegrain locking or atomic
                                        // variables in segment
  if (region->segments[segment_index].deregistered ==
      NONE) { // if segment is not deregisterd then deregister it
    region->segments[segment_index].deregistered = tx;
  } else {
    lock_release(&(region->global_lock));
    // if has already been deregistered then abort the transacation
    if (region->segments[segment_index].deregistered != tx) {
      return (abort_transaction_tx(region, tx));
    }
  }
  lock_release(&(region->global_lock));
  return true;
}

// Like in project description, slightly simplified the if/else to something i
// understood better but outcome is the same
alloc_t read_word(segment_t *segment, tx_t tx, bool is_ro, int index,
                  void *target) {
  if (is_ro == true) {
    read_read_only_copy(index, target, segment);
    return success_alloc;
  } else { // r_w transacation
    lock_acquire(&segment->control_lock[index]);
    // if word not written, can read the r_o copy
    control_t *control = &segment->control[index];
    if (control->word_has_been_written == false) {
      // if first access add myself to access_set
      if (control->access_set == NONE) {
        control->access_set = tx;
      }
      lock_release(
          &segment->control_lock[index]); // allow parallel reads on same word
      read_read_only_copy(index, target, segment);
      return success_alloc;
    }

    // if word written in current epoch by "this" transcation then can read,
    // else abort
    if (control->word_has_been_written == true && control->access_set == tx) {
      lock_release(
          &segment->control_lock[index]); // allow parallel reads on same word
      // read the writable copy
      read_writable_copy(index, target, segment);
      return success_alloc;
    } else {
      lock_release(&segment->control_lock[index]);
      return abort_alloc;
    }
  }
}

// Exactly like Project description
alloc_t write_word(segment_t *segment, tx_t tx, int index, const void *source) {
  lock_acquire(&segment->control_lock[index]);

  control_t *control = &segment->control[index];
  if (control->word_has_been_written == true) {
    lock_release(&segment->control_lock[index]); // allow concurrent write
    if (control->access_set == tx) {
      write_to_correct_copy(index, source, segment);
      return success_alloc;
    } else {
      return abort_alloc;
    }
  } else { // abort if word has already been accessed by another tx
    if (control->access_set != NONE && control->access_set != tx) {
      lock_release(&segment->control_lock[index]);
      return abort_alloc;
    } else { // CASE: first to access and write to word. So update
             // datastructures and release lock to allow concurent writes
      control->access_set = tx;
      control->word_has_been_written = true; // set is_written flag to true;
      lock_release(&segment->control_lock[index]); // allow concurrent writes

      write_to_correct_copy(index, source, segment);
      return success_alloc;
    }
  }
}

// always returns false
bool abort_transaction_tx(shared_t shared, tx_t tx) {
  region_t *region = (region_t *)shared;
  segment_t *segment;
  for (int i = 0; i < region->n_segments; i++) {
    segment = &region->segments[i];
    // Check words this tx accessed and wrote to and and make those words
    // available again
    for (size_t i = 0; i < segment->n_words;
         i++) { // TODO: Can optimize by using stack of modified
                // word_indexes instead of iterating over all words and
                // checking if they have been written
      if (segment->control[i].access_set == tx &&
          segment->control[i].word_has_been_written == true) {
        lock_acquire(&segment->control_lock[i]);
        segment->control[i].access_set = NONE;
        segment->control[i].word_has_been_written = false;
        lock_release(&segment->control_lock[i]);
      }
    }
    lock_acquire(&region->global_lock);
    if (segment->deregistered == tx) {
      segment->deregistered = NONE;
    }
    lock_release(&region->global_lock);
  }
  leave_batcher(region, tx);
  return false;
}
