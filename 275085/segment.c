#include "segment.h"

// init segment
bool segment_init(segment_t *segment, tx_t tx, size_t size, size_t align) {
  segment->n_words = size / (align);
  segment->align = align;
  segment->tx_id_of_creator = tx;
  atomic_store(&segment->deregistered, INVALID_TX);

  segment->cp_is_ro = calloc(segment->n_words, sizeof(int));
  if (!segment->cp_is_ro) {
    return false;
  }

  segment->cp0 = calloc(segment->n_words, align);
  if (!segment->cp0) {
    free(segment->cp_is_ro);
    return false;
  }
  segment->cp1 = calloc(segment->n_words, align);
  if (!segment->cp1) {
    free(segment->cp_is_ro);
    free(segment->cp0);
    return false;
  }

  // allocate access set and init to -1
  segment->access_set = malloc(segment->n_words * sizeof(tx_t));
  if (!segment->access_set) {
    free(segment->cp0);
    free(segment->cp1);
    free(segment->cp_is_ro);
    return false;
  }

  for (size_t i = 0; i < segment->n_words; i++) {
    segment->access_set[i] = INVALID_TX;
  }
  // memset(segment->access_set, INVALID_TX,
  // ((int)segment->n_words)*sizeof(tx_t));

  // allocate and init to false array of boolean flags indicating if a word has
  // been written in the epoch
  segment->is_written_in_epoch = (bool *)calloc(segment->n_words, sizeof(bool));
  if (!segment->is_written_in_epoch) {
    free(segment->cp0);
    free(segment->cp1);
    free(segment->cp_is_ro);
    free(segment->access_set);
    return false;
  }

  // allocate and init array of locks for words
  segment->word_locks = malloc(segment->n_words * sizeof(struct lock_t));
  if (!segment->is_written_in_epoch) {
    free(segment->is_written_in_epoch);
    free(segment->cp0);
    free(segment->cp1);
    free(segment->cp_is_ro);
    free(segment->access_set);
    return false;
  }
  for (size_t i = 0; i < segment->n_words; i++) {
    if (!lock_init(&(segment->word_locks[i]))) {
      free(segment->word_locks);
      free(segment->is_written_in_epoch);
      free(segment->cp0);
      free(segment->cp1);
      free(segment->cp_is_ro);
      free(segment->access_set);
      return false;
    }
  }

  return true;
}

// create get virtual address from a segment id
void *get_virt_addr(int seg_id) {
  // address is (NUM_SEGMENT + 1) << 24 + word offset
  return (void *)((intptr_t)((++seg_id) << SEGMENT_SHIFT));
}

int extract_word_index_from_virt_addr(void const *addr, size_t align) {
  intptr_t tmp = (intptr_t)addr >> SEGMENT_SHIFT;
  intptr_t shifted_segment_id = tmp << SEGMENT_SHIFT;
  int word_offset = (intptr_t)addr - shifted_segment_id;
  return word_offset / align; // word_offset/align gives word_index
}

int extract_seg_id_from_virt_addr(void const *addr) {
  intptr_t num_s = (intptr_t)addr >> SEGMENT_SHIFT;
  return num_s - 1;
}