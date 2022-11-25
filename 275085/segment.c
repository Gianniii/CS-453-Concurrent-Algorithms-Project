#include "segment.h"

bool init_segment(segment_t *segment, size_t align, size_t size) {
  segment->align = align;
  segment->n_words = size / (align);
  segment->deregistered = NONE;

  segment->words_array_A = calloc(segment->n_words, align);
  if (!segment->words_array_A) {
    return false;
  }
  segment->words_array_B = calloc(segment->n_words, align);
  if (!segment->words_array_B) {
    free(segment->words_array_A);
    return false;
  }

  segment->control = calloc(segment->n_words, sizeof(control_t));
  if(!segment->control) {
    free(segment->words_array_A);
    free(segment->words_array_B);
    return false;
  }

  segment->control_lock = malloc(segment->n_words * sizeof(struct lock_t));
  if (!segment->control_lock) {
    free(segment->control);
    free(segment->words_array_A);
    free(segment->words_array_B);
    return false;
  }
  for (size_t i = 0; i < segment->n_words; i++) {
    segment->control[i].access_set = NONE;
    lock_init(&(segment->control_lock[i]));
  }

  return true;
}

// address format first 8 bits bits (segment_id + 1) and remaning bits word offset
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
    lock_cleanup(&(s->control_lock[i]));
  }
  free(s->control_lock);
  free(s->control);
  free(s->words_array_A);
  free(s->words_array_B);
}