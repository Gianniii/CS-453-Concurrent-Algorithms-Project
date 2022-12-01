#include "segment.h"

#define VIRT_ADDR_OFFSET (intptr_t)(0x8000000) //offset for base of virtual addr
#define WORD_ADDR_SPACE_BITS 16;

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

// first 16 msb are used for word address, the remaning bits are used to store segment id with and offset(because addr 0 not allowed)
// get virtual address from a segment id
void *get_virt_addr(int seg_id) {
  return (void *)((intptr_t)((VIRT_ADDR_OFFSET | seg_id) << 16));
}

int extract_word_index_from_virt_addr(void const *addr, size_t align) {
  int word_offset = (intptr_t)addr & (intptr_t)(0xFFFF);
  return word_offset / align; // return id of word
}

int extract_seg_id_from_virt_addr(void const *addr) {
  return VIRT_ADDR_OFFSET ^ (intptr_t)((intptr_t)addr >> 16);
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