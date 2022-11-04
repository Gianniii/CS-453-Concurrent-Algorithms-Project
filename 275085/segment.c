#include "segment.h"

//init segment
bool segment_init(segment_t *segment, tx_t tx, size_t size, size_t align) {
  segment->num_words = size / (align);
  segment->align = align;
  segment->created_by_tx = tx;
  atomic_store(&segment->to_delete, INVALID_TX);
  atomic_store(&segment->num_writen_words, 0);

  // alloc words in segment
  segment->copy_0 = calloc(segment->num_words, align);
  if (!segment->copy_0) {
    return false;
  }
  segment->copy_1 = calloc(segment->num_words, align);
  if (!segment->copy_1) {
    free(segment->copy_0);
    return false;
  }
  // init supporting data structure for words (to 0)
  segment->read_only_copy = calloc(segment->num_words, sizeof(int));
  if (!segment->read_only_copy) {
    free(segment->copy_0);
    free(segment->copy_1);

    return false;
  }

  // allocate access set and init to -1
  segment->access_set = malloc(segment->num_words * sizeof(tx_t));
  if (!segment->access_set) {
    free(segment->copy_0);
    free(segment->copy_1);
    free(segment->read_only_copy);
    return false;
  }
  for (int i = 0; i < (int)segment->num_words; i++) {
    segment->access_set[i] = INVALID_TX;
  }
  //memset(segment->access_set, INVALID_TX, ((int)segment->num_words)*sizeof(tx_t));

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
  segment->word_locks = malloc(segment->num_words * sizeof(struct lock_t));
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

//create get virtual address from a segment id
void *get_virt_addr(int seg_id) {
  // address is (NUM_SEGMENT + 1) << 24 + offset word
  return (void *)((intptr_t)((++seg_id) << SEGMENT_SHIFT));
}

int extract_word_num_from_virt_addr(void const * addr) {
  intptr_t tmp = (intptr_t)addr >> SEGMENT_SHIFT;
  intptr_t shifted_segment_id = tmp << SEGMENT_SHIFT;
  return (intptr_t)addr - shifted_segment_id;
}

int extract_seg_id_from_virt_addr(void const* addr) {
  intptr_t num_s = (intptr_t)addr >> SEGMENT_SHIFT;
  return num_s -1;
}