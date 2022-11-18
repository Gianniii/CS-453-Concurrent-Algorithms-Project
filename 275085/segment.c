#include "segment.h"

bool init_segment(segment_t *segment, size_t align, size_t size) {
  segment->align = align;
  segment->n_words = size / (align);
  segment->deregistered = NONE;

  segment->word_is_ro = calloc(segment->n_words, sizeof(int));
  if (!segment->word_is_ro) {
    return false;
  }

  segment->words_array_A = calloc(segment->n_words, align);
  if (!segment->words_array_A) {
    return false;
  }
  segment->words_array_B = calloc(segment->n_words, align);
  if (!segment->words_array_B) {
    free(segment->words_array_A);
    return false;
  }

  segment->access_set = malloc(segment->n_words * sizeof(tx_t));
  if (!segment->access_set) {
    free(segment->words_array_A);
    free(segment->words_array_B);
    return false;
  }

  for (size_t i = 0; i < segment->n_words; i++) {
    segment->access_set[i] = NONE;
  }

  segment->control = calloc(segment->n_words, sizeof(control_t));
  if(!segment->control) {
    free(segment->access_set);
    free(segment->words_array_A);
    free(segment->words_array_B);
    free(segment->word_is_ro);
    return false;
  }

  segment->word_has_been_written_flag =
      (bool *)calloc(segment->n_words, sizeof(bool));
  if (!segment->word_has_been_written_flag) {
    free(segment->words_array_A);
    free(segment->control);
    free(segment->words_array_B);
    free(segment->word_is_ro);
    free(segment->access_set);
    return false;
  }

  segment->word_lock = malloc(segment->n_words * sizeof(struct lock_t));
  if (!segment->word_lock) {
    free(segment->control);
    free(segment->word_has_been_written_flag);
    free(segment->words_array_A);
    free(segment->words_array_B);
    free(segment->word_is_ro);
    free(segment->access_set);
    return false;
  }
  for (size_t i = 0; i < segment->n_words; i++) {
    if (!lock_init(&(segment->word_lock[i]))) {
      free(segment->control);
      free(segment->word_lock);
      free(segment->word_has_been_written_flag);
      free(segment->words_array_A);
      free(segment->words_array_B);
      free(segment->word_is_ro);
      free(segment->access_set);
      return false;
    }
  }

  return true;
}

// address format (segment_id + 1) << shift_constant + word offset
// get virtual address from a segment id
void *get_virt_addr(int seg_id) {
  return (void *)((intptr_t)((++seg_id) << 24));
}

int extract_word_index_from_virt_addr(void const *addr, size_t align) {
  intptr_t tmp = (intptr_t)addr >> 24;
  intptr_t shifted_segment_id = tmp << 24;
  int word_offset = (intptr_t)addr - shifted_segment_id;
  return word_offset / align; // return id of word
}

int extract_seg_id_from_virt_addr(void const *addr) {
  intptr_t num_s = (intptr_t)addr >> 24;
  return num_s - 1;
}

void segment_destroy(segment_t *s) {
  for (size_t i = 0; i < s->n_words; i++) {
    lock_cleanup(&(s->word_lock[i]));
  }
  free(s->word_lock);
  free(s->word_has_been_written_flag);
  free(s->word_is_ro);
  free(s->access_set);
  free(s->control);
  free(s->words_array_A);
  free(s->words_array_B);
}