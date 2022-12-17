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
  if (!segment->control) {
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

void segment_destroy(segment_t *s) {
  for (size_t i = 0; i < s->n_words; i++) {
    lock_cleanup(&(s->control_lock[i]));
  }
  free(s->control_lock);
  free(s->control);
  free(s->words_array_A);
  free(s->words_array_B);
}