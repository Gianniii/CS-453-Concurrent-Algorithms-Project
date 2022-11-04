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
#define SEGMENT_SHIFT 24
#define INIT_FREED_SEG_SIZE \
  10 // better if it grows together with the segment array
#define INIT_SEG_SIZE \
  10 // 1 if you want reallocation of segments (not statically init)
#define INVALID_TX UINT_MAX

/** segment structure (multiple per shared memory).
 * @param created_by_tx If -1 segment is shared, else it's temporary and
 *must be deleted if tx abort
 * @param to_delete If set to some tx, the segment has to be deleted when
 *the last transaction exit the batcher, rollback set to 0 if the tx
 *rollback
 * @param has_been_modified Flag to track if segment has been modified in
 *epoch
 * @param index_modified_words Array to store sequential indexes of accessed
 *words in segment
 * @param cnt_index_modified_words Atomic counter incremented every time
 *there is an operation on word
 **/
typedef struct segment_s
{
  size_t num_words;          // num words in segment
  void *copy_0;              // Copy 0 of segments words (accessed shifting a pointer)
  void *copy_1;              // Copy 1 of segments words (accessed shifting a pointer)
  int *read_only_copy;       // Array of flags for read-only copy
  tx_t *access_set;          // Array of read-write tx which have accessed the word (the
                             // first to access the word(read or write) will own it for
                             // the epoch)
  bool *is_written_in_epoch; // Array of boolean to flag if the word has been
                             // written
  struct lock_t *word_locks;
  int align;               // size of a word
  tx_t created_by_tx;      // in tm_alloc
  _Atomic(tx_t) to_delete; // in tm_free
  bool has_been_modified;
  int *index_modified_words;
  _Atomic(int) num_writen_words;
} segment_t;

bool segment_init(segment_t *, tx_t, size_t, size_t);
void *get_virt_addr(int);
int extract_word_num_from_virt_addr(void const *addr);
int extract_seg_id_from_virt_addr(void const *addr);

alloc_t read_word(int, void *, segment_t *, bool, tx_t);
alloc_t write_word(int, const void *, segment_t *, tx_t);
