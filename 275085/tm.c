/**
 * @file   tm.c
 * @author Gianni Lodetti
 *
 * @section LICENSE
 *
 * @section DESCRIPTION
 *
 * Implementation of my own transaction manager.
 * You can completely rewrite this file (and create more files) as you wish.
 * Only the interface (i.e. exported symbols and semantic) must be preserved.
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

// External headers
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Internal headers
#include <tm.h>

#include "macros.h"

//MACROS ----------------------------------------------------------------------------------------------------------------------

/** Define a proposition as likely true.
 * @param prop Proposition
**/
#undef likely
#ifdef __GNUC__
    #define likely(prop) \
        __builtin_expect((prop) ? true : false, true /* likely */)
#else
    #define likely(prop) \
        (prop)
#endif

/** Define a proposition as likely false.
 * @param prop Proposition
**/
#undef unlikely
#ifdef __GNUC__
    #define unlikely(prop) \
        __builtin_expect((prop) ? true : false, false /* unlikely */)
#else
    #define unlikely(prop) \
        (prop)
#endif

/** Define a variable as unused.
**/
#undef unused
#ifdef __GNUC__
    #define unused(variable) \
        variable __attribute__((unused))
#else
    #define unused(variable)
    #warning This compiler has no support for GCC attributes
#endif

//End Macros --------------------------------------------------------------------------------------------------------
//Locks --------------------------------------------------------------------------------------------------------------

/**
 * @brief A lock that can only be taken exclusively. Contrarily to shared locks,
 * exclusive locks have wait/wake_up capabilities.
 */
struct lock_t {
    pthread_mutex_t mutex;
    pthread_cond_t cv;
};

/** Initialize the given lock.
 * @param lock Lock to initialize
 * @return Whether the operation is a success
**/
bool lock_init(struct lock_t* lock) {
    return pthread_mutex_init(&(lock->mutex), NULL) == 0
        && pthread_cond_init(&(lock->cv), NULL) == 0;
}

/** Clean up the given lock.
 * @param lock Lock to clean up
**/
void lock_cleanup(struct lock_t* lock) {
    pthread_mutex_destroy(&(lock->mutex));
    pthread_cond_destroy(&(lock->cv));
}

/** Wait and acquire the given lock.
 * @param lock Lock to acquire
 * @return Whether the operation is a success
**/
bool lock_acquire(struct lock_t* lock) {
    return pthread_mutex_lock(&(lock->mutex)) == 0;
}

/** Release the given lock.
 * @param lock Lock to release
**/
void lock_release(struct lock_t* lock) {
    pthread_mutex_unlock(&(lock->mutex));
}

/** Wait until woken up by a signal on the given lock.
 *  The lock is released until lock_wait completes at which point it is acquired
 *  again. Exclusive lock access is enforced.
 * @param lock Lock to release (until woken up) and wait on.
**/
void lock_wait(struct lock_t* lock) {
    pthread_cond_wait(&(lock->cv), &(lock->mutex));
}

/** Wake up all threads waiting on the given lock.
 * @param lock Lock on which other threads are waiting.
**/
void lock_wake_up(struct lock_t* lock) {
    pthread_cond_broadcast(&(lock->cv));
}

//End Locks-------------------------------------------------------------------------------------------------------
//Implementation -------------------------------------------------------------------------------------------------
static const tx_t read_only_tx  = UINTPTR_MAX - 10;
static const tx_t read_write_tx = UINTPTR_MAX - 11;

/**
 * @brief List of dynamically allocated segments.
 */
struct segment_node {
    struct segment_node* prev;
    struct segment_node* next;
    // uint8_t segment[] // segment of dynamic size
};
typedef struct segment_node* segment_list;

/**
 * @brief Simple Shared Memory Region (a.k.a Transactional Memory).
 */
struct region {
    unsigned long num_ro_tx; //Num currently executing read-only tx's
    unsigned long num_rw_tx; //Num currently executing r-w tx's
    struct lock_t write_lock; // Global (coarse-grained) lock
    struct lock_t meta_lock; // Global lock for metadata changes
    void* start;        // Start of the shared memory region (i.e., of the non-deallocable memory segment)
    segment_list allocs; // Shared memory segments dynamically allocated via tm_alloc within transactions
    size_t size;        // Size of the non-deallocable memory segment (in bytes)
    size_t align;       // Size of a word in the shared memory region (in bytes)
};


/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
**/
shared_t tm_create(size_t size, size_t align) {
    struct region* region = (struct region*) malloc(sizeof(struct region));
    if (unlikely(!region)) {
        return invalid_shared;
    }
    // We allocate the shared memory buffer such that its words are correctly
    // aligned.
    if (posix_memalign(&(region->start), align, size) != 0) {
        free(region);
        return invalid_shared;
    }
    if (!lock_init(&(region->write_lock))) {
        free(region->start);
        free(region);
        return invalid_shared;
    }
    if (!lock_init(&(region->meta_lock))) {
        free(region->start);
        lock_cleanup(&(region->write_lock));
        free(region);
        return invalid_shared;
    }
    
    memset(region->start, 0, size);
    region->num_ro_tx = 0;
    region->num_rw_tx = 0;
    region->allocs      = NULL;
    region->size        = size;
    region->align       = align;
    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared) {
    // Note: To be compatible with any implementation, shared_t is defined as a
    // void*. For this particular implementation, the "real" type of a shared_t
    // is a struct region*.
    struct region* region = (struct region*) shared;
    while (region->allocs) { // Free allocated segments
        segment_list tail = region->allocs->next;
        free(region->allocs);
        region->allocs = tail;
    }
    free(region->start);
    lock_cleanup(&(region->write_lock));
    lock_cleanup(&(region->meta_lock));
    free(region);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void* tm_start(shared_t shared) {
    return ((struct region*) shared)->start;
}


/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t shared) {
    return ((struct region*) shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t shared) {
    return ((struct region*) shared)->align;
}


/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared, bool is_ro) {
    struct region *region = (struct region*)shared;
    lock_acquire(&(region->meta_lock)); //TODO modify, also what is this lock for?
    
    
    if (is_ro) {
        //white for write transactions to finish
        while(region->num_rw_tx != 0UL) {
            lock_wait(&(region->meta_lock));
        }
        region->num_ro_tx += 1UL;
        lock_release(&(region->meta_lock));
        return read_only_tx;
    } else {
        //wait for read only transactions to finish
        while(region->num_ro_tx != 0UL){
            lock_wait(&(region->meta_lock));
        }
        region->num_rw_tx += 1UL;

        lock_release(&(region->meta_lock));
        lock_acquire(&(region->write_lock)); //global write lock wtf?? how was this even considered as implementation??
        return read_write_tx;
    }
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared, tx_t tx) {
    struct region *region = (struct region*)shared;

    if (tx == read_only_tx) {
        lock_acquire((&region->meta_lock));
        region->num_ro_tx -= 1UL;
        lock_release(&(region->meta_lock));
        lock_wake_up(&(region->meta_lock));
    } else {
        lock_release(&region->write_lock);
        lock_acquire(&(region->meta_lock));
        region->num_rw_tx -= 1UL;
        lock_release(&(region->meta_lock));
        lock_wake_up(&(region->meta_lock));
    }
    return true;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t unused(shared), tx_t unused(tx), void const* source, size_t size, void* target) {
    memcpy(target, source, size);
    return true;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t unused(shared), tx_t unused(tx), void const* source, size_t size, void* target) {
    memcpy(target, source, size);
    return true;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t tm_alloc(shared_t shared, tx_t unused(tx), size_t size, void** target) {
    // TODO: tm_alloc(shared_t, tx_t, size_t, void**)
    size_t align = ((struct region*) shared)->align;
    align = align < sizeof(struct segment_node*) ? sizeof(void*) : align;

    struct segment_node* sn;
    if (unlikely(posix_memalign((void**)&sn, align, sizeof(struct segment_node) + size) != 0)) // Allocation failed
        return nomem_alloc;

    // Insert in the linked list
    sn->prev = NULL;
    sn->next = ((struct region*) shared)->allocs;
    if (sn->next) sn->next->prev = sn;
    ((struct region*) shared)->allocs = sn;

    void* segment = (void*) ((uintptr_t) sn + sizeof(struct segment_node));
    memset(segment, 0, size);
    *target = segment;
    return success_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t shared, tx_t unused(tx), void* segment) {
    struct segment_node* sn = (struct segment_node*) ((uintptr_t) segment - sizeof(struct segment_node));

    // Remove from the linked list
    if (sn->prev) sn->prev->next = sn->next;
    else ((struct region*) shared)->allocs = sn->next;
    if (sn->next) sn->next->prev = sn->prev;

    free(sn);
    return true;
}
