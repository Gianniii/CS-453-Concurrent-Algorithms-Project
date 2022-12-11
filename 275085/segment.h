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
  bool word_is_ro; // Like in description: Flag indicating which word is read
                   // only
  tx_t access_set; // Like in Description: shows tx which have
                   // accessed the word, first to access will own it for it for
                   // the epoch
  bool word_has_been_written; // Like in project descrition : boolean to
                              // flag if the word has been written
} control_t;

typedef struct {
  control_t *control;          // Control structure like in project description
  struct lock_t *control_lock; // used because to lazy to use atomic variables
  void *words_array_A; // Like in Description: first copy from description
  void *words_array_B; // Like in Description: second copy from description
  size_t n_words;
  int align;         // size of a word
  tx_t deregistered; // to be freed in tm_free(equals NONE if not set else it
                     // equals the tx that deregistered it)
} segment_t;

bool init_segment(segment_t *seg, size_t size, size_t align);
void segment_destroy(segment_t *s);

alloc_t read_word(segment_t *segment, tx_t tx, bool is_ro, int index,
                  void *target);
alloc_t write_word(segment_t *segment, tx_t tx, int index, const void *target);
