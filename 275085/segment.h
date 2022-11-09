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
#define SEGMENT_SHIFT 24 // TODO
#define INIT_FREED_SEG_SIZE 0x10000
#define INIT_SEG_SIZE 10
#define INVALID_TX UINT_MAX

/** segment structure (multiple per shared memory).
 * @param to_delete If set to some tx, the segment has to be deleted when
 *the last transaction exit the batcher, rollback set to 0 if the tx
 *rollback
 **/
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
  struct lock_t *word_locks;
  int align;               // size of a word
  tx_t tx_id_of_creator;   // in tm_alloc  TODO CHANGE ME
  _Atomic(tx_t) to_delete; // in tm_free   TODO CHANGE ME
  // stack_t modified_word_indexes       //Potential optimization to avoid
  // iterating over all words and checking if they have been written in epoch
} segment_t;

bool segment_init(segment_t *seg, size_t size, size_t align, tx_t tx);
void *get_virt_addr(int);
int extract_word_index_from_virt_addr(void const *addr, size_t align);
int extract_seg_id_from_virt_addr(void const *addr);

alloc_t read_word(int, void *, segment_t *, bool, tx_t);
alloc_t write_word(int, const void *, segment_t *, tx_t);
