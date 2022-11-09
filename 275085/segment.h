#pragma once

#include "lock.h"
#include "macros.h"
#include "tm.h"
#include <limits.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// Segment constants
#define SEGMENT_SHIFT 24 //TODO
#define INIT_FREED_SEG_SIZE 0x10000
#define INIT_SEG_SIZE 10
#define INVALID_TX UINT_MAX

/** segment structure (multiple per shared memory).
 * @param to_delete If set to some tx, the segment has to be deleted when
 *the last transaction exit the batcher, rollback set to 0 if the tx
 *rollback
 * @param has_been_modified Flag to track if segment has been modified in
 *epoch
 * @param index_modified_words Array to store sequential indexes of accessed  //THIS IS AN OPTIMIZATION COULD BE REPLACE BY STACK
 * //OR NOT NECESSARY AT ALL!!
 *words in segment
 * @param cnt_index_modified_words Atomic counter incremented every time
 *there is an operation on word
 **/
typedef struct {
  size_t n_words;       
  void* cp0;              // Copy 0 of segments words (accessed shifting a pointer)
  void *cp1;              // Copy 1 of segments words (accessed shifting a pointer)
  int * cp_is_ro;       // Array of flags for read-only copy
  tx_t *access_set;          // Array of read-write tx which have accessed the word (the
                             // first to access the word(read or write) will own it for
                             // the epoch)
  bool *is_written_in_epoch; // Array of boolean to flag if the word has been
                             // written
  struct lock_t *word_locks;
  int align;               // size of a word
  tx_t tx_id_of_creator;      // in tm_alloc
  _Atomic(tx_t) to_delete; // in tm_free
  int *index_modified_words; 
  _Atomic(int) num_writen_words;
} segment_t;

bool segment_init(segment_t *seg, size_t size, size_t align, tx_t tx);
void *get_virt_addr(int);
int extract_word_index_from_virt_addr(void const *addr, size_t align);
int extract_seg_id_from_virt_addr(void const *addr);

alloc_t read_word(int, void *, segment_t *, bool, tx_t);
alloc_t write_word(int, const void *, segment_t *, tx_t);
