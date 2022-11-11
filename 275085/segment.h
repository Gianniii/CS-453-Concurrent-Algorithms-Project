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

// Segment constants TODO CHANGE ALL
#define SEGMENT_SHIFT 24
#define MAX_NUM_SEGMENTS 0x10000    
#define NOT_FREE false
#define FREE true
static const tx_t NONE = 0xFFFFF;
/**typedef struct {
  int cp_is_ro;
  bool is_written_in_epoch
} control_t;*/
// or contro_t // control structure for each word!!!! // idea wouldnt need
// to use several array(cp0, cp1, cp_is_ro,
// access_set, is_written_in_epoch, word_locks) but one array of
// words[word_index]
typedef struct {
  size_t n_words;
  void *cp0;        // Like in Description: first copy from description
  void *cp1;        // Like in Description: second copy from description
  int *cp_is_ro;    // Like in Description: Array of flags for read-only copy
  tx_t *access_set; // Like in Description: Array of read-write tx which have
                    // accessed the word (the first to access the word(read or
                    // write) will own it for the epoch)
  bool *is_written_in_epoch; // Like in project descrition : Array of boolean to
                             // flag if the word has been written
  struct lock_t *word_locks; // used because to lazy to use atomic variables
  int align;                 // size of a word
  tx_t tx_id_of_creator;    
  tx_t deregistered; // to be freed in tm_free(equals NONE if not set else it equals the tx that deregistered it)
  // stack_t modified_word_indexes       //Potential optimization to avoid
  // iterating over all words and checking if they have been written in epoch
} segment_t;

bool segment_init(segment_t *seg, size_t size, size_t align, tx_t tx);
void *get_virt_addr(int);
int extract_word_index_from_virt_addr(void const *addr, size_t align);
int extract_seg_id_from_virt_addr(void const *addr);

alloc_t read_word(int word_idx, void * target, segment_t *segment, bool, tx_t tx);
alloc_t write_word(int word_idx, const void *target, segment_t *segment, tx_t tx);
