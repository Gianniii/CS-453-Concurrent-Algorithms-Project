#pragma once

#include "lock.h"
#include "macros.h"
#include "stack.h"
#include "tm.h"
#include <limits.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NUM_SEGMENTS 0x10000 // From project description
#define NOT_FREE false
#define FREE true
static const tx_t NONE = 0xFFFFF;

typedef struct {
  int word_is_ro; //Like in description: Flag indicating which word is read only
  tx_t access_set; // Like in Description: shows tx which have
                    // accessed the word, first to access will own it for it for the epoch
  //bool word_has_been_written_flag
} control_t;
// or contro_t // control structure for each word!!!! // idea wouldnt need
// to use several array(words_array_A, words_array_B, word_is_ro,
// access_set, word_has_been_written_flag, word_lock) but one array of
// words[word_index]
typedef struct {
  control_t* control;       //Control structure like in project description
  struct lock_t *word_lock; // used because to lazy to use atomic variables
  void *words_array_A; // Like in Description: first copy from description
  void *words_array_B; // Like in Description: second copy from description
  size_t n_words;
  bool *word_has_been_written_flag; // Like in project descrition : Array of
                                    // boolean to flag if the word has been
                                    // written
  int align;         // size of a word
  tx_t deregistered; // to be freed in tm_free(equals NONE if not set else it
                     // equals the tx that deregistered it)
} segment_t;

bool init_segment(segment_t *seg, size_t size, size_t align);
void *get_virt_addr(int);
int extract_word_index_from_virt_addr(void const *addr, size_t align);
int extract_seg_id_from_virt_addr(void const *addr);
void segment_destroy(segment_t *s);

alloc_t read_word(segment_t *segment, tx_t tx, bool is_ro, int index,
                  void *target);
alloc_t write_word(segment_t *segment, tx_t tx, int index, const void *target);
