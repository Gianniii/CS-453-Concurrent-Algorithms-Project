#include "segment.h"

bool segment_init(segment_t *segment, tx_t tx, size_t size, size_t alignment) {
  segment->num_words = size / (alignment);
  segment->align = alignment;
  segment->created_by_tx = tx;
  segment->has_been_modified = false;
  atomic_store(&segment->to_delete, INVALID_TX);
  atomic_store(&segment->num_writen_words, 0);

  // alloc words in segment
  segment->copy_0 = (void *)calloc(segment->num_words, alignment);
  if (!segment->copy_0) {
    return false;
  }
  segment->copy_1 = (void *)calloc(segment->num_words, alignment);
  if (!segment->copy_1) {
    free(segment->copy_0);
    return false;
  }
  // init supporting data structure for words (to 0)
  segment->read_only_copy = (int *)calloc(segment->num_words, sizeof(int));
  if (!segment->read_only_copy) {
    free(segment->copy_0);
    free(segment->copy_1);

    return false;
  }

  // allocate access set and init to -1
  segment->access_set = (tx_t *)malloc(segment->num_words * sizeof(tx_t));
  if (!segment->access_set) {
    free(segment->copy_0);
    free(segment->copy_1);
    free(segment->read_only_copy);
    return false;
  }
  for (int i = 0; i < (int)segment->num_words; i++) {
    segment->access_set[i] = INVALID_TX;
  }

  // allocate and init to false array of boolean flags indicating if a word has
  // been written in the epoch
  segment->is_written_in_epoch =
      (bool *)calloc(segment->num_words, sizeof(bool));
  if (!segment->is_written_in_epoch) {
    free(segment->copy_0);
    free(segment->copy_1);
    free(segment->read_only_copy);
    free(segment->access_set);
    return false;
  }

  // set array of indexes of modified words
  segment->index_modified_words =
      (int *)calloc(segment->num_words, sizeof(int));
  if (!segment->index_modified_words) {
    free(segment->is_written_in_epoch);
    free(segment->copy_0);
    free(segment->copy_1);
    free(segment->read_only_copy);
    free(segment->access_set);
    return false;
  }

  // allocate and init array of locks for words
  segment->word_locks = (lock_t *)malloc(segment->num_words * sizeof(lock_t));
  if (!segment->is_written_in_epoch) {
    free(segment->index_modified_words);
    free(segment->is_written_in_epoch);
    free(segment->copy_0);
    free(segment->copy_1);
    free(segment->read_only_copy);
    free(segment->access_set);
    return false;
  }
  for (int i = 0; i < (int)segment->num_words; i++) {
    if (!lock_init(&(segment->word_locks[i]))) {
      free(segment->word_locks);
      free(segment->is_written_in_epoch);
      free(segment->copy_0);
      free(segment->copy_1);
      free(segment->read_only_copy);
      free(segment->access_set);
      return false;
    }
  }

  return true;
}

/** Encode segment number into an opaque pointer address.
 * @param segment_num Number of segment
 * @return Encoded address
 **/
void *encode_segment_address(int segment_num) {
  // address is (NUM_SEGMENT + 1) << 24 + offset word
  // << means shift left
  segment_num++;
  intptr_t addr = (segment_num << SEGMENT_SHIFT);
  return (void *)addr;
}

/** Decode opaque pointer into segment and word number.
 * @param addr Opaque pointer
 * @param num_segment Pointer to segment number
 * @param num_word Pointer to word number
 * @return Decoded Address
 **/
void decode_segment_address(void const *addr, int *num_segment, int *num_word) {
  intptr_t num_s, num_w;

  // calculate word and segment number
  num_s = (intptr_t)addr >> SEGMENT_SHIFT;
  intptr_t difference = num_s << SEGMENT_SHIFT;
  num_w = (intptr_t)addr - difference;

  // save calculated segment number - 1 and word number
  *num_segment = num_s - 1;
  *num_word = num_w;
}