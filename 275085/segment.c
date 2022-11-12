#include "segment.h"

// init segment
bool segment_init(segment_t *segment, tx_t tx, size_t size, size_t align) {
  segment->n_words = size / (align);
  segment->align = align;
  segment->tx_id_of_creator = tx;
  segment->deregistered = NONE;

  segment->word_is_ro = calloc(segment->n_words, sizeof(int));
  if (!segment->word_is_ro) {
    return false;
  }

  segment->words_array_A = calloc(segment->n_words, align);
  if (!segment->words_array_A) {
    free(segment->word_is_ro);
    return false;
  }
  segment->words_array_B = calloc(segment->n_words, align);
  if (!segment->words_array_B) {
    free(segment->word_is_ro);
    free(segment->words_array_A);
    return false;
  }

  // allocate access set and init to -1
  segment->access_set = malloc(segment->n_words * sizeof(tx_t));
  if (!segment->access_set) {
    free(segment->words_array_A);
    free(segment->words_array_B);
    free(segment->word_is_ro);
    return false;
  }

  for (size_t i = 0; i < segment->n_words; i++) {
    segment->access_set[i] = NONE;
  }
  // memset(segment->access_set, INVALID_TX,
  // ((int)segment->n_words)*sizeof(tx_t));

  // allocate and init to false array of boolean flags indicating if a word has
  // been written in the epoch
  segment->is_written_in_epoch = (bool *)calloc(segment->n_words, sizeof(bool));
  if (!segment->is_written_in_epoch) {
    free(segment->words_array_A);
    free(segment->words_array_B);
    free(segment->word_is_ro);
    free(segment->access_set);
    return false;
  }

  // allocate and init array of locks for words
  segment->word_locks = malloc(segment->n_words * sizeof(struct lock_t));
  if (!segment->is_written_in_epoch) {
    free(segment->is_written_in_epoch);
    free(segment->words_array_A);
    free(segment->words_array_B);
    free(segment->word_is_ro);
    free(segment->access_set);
    return false;
  }
  for (size_t i = 0; i < segment->n_words; i++) {
    if (!lock_init(&(segment->word_locks[i]))) {
      free(segment->word_locks);
      free(segment->is_written_in_epoch);
      free(segment->words_array_A);
      free(segment->words_array_B);
      free(segment->word_is_ro);
      free(segment->access_set);
      return false;
    }
  }

  return true;
}

// encoding of addr (segment_id + 1) << shift_constant + word offset
// create get virtual address from a segment id
void *get_virt_addr(int seg_id) {
  return (void *)((intptr_t)((++seg_id) << 24));
}

int extract_word_index_from_virt_addr(void const *addr, size_t align) {
  intptr_t tmp = (intptr_t)addr >> 24;
  intptr_t shifted_segment_id = tmp << 24;
  int word_offset = (intptr_t)addr - shifted_segment_id;
  return word_offset / align; // word_offset/align gives word_index
}

int extract_seg_id_from_virt_addr(void const *addr) {
  intptr_t num_s = (intptr_t)addr >> 24;
  return num_s - 1;
}