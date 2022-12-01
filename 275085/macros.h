#include <stdbool.h>


//CODE FOR VIRTUAL ADDRESS TRANSLATION-------------------------------------------------------------------
#define VIRT_ADDR_OFFSET (intptr_t)(0x8000000) //offset for base of virtual addr
#define WORD_ADDR_SPACE_BITS 16;
#define LSB16 (intptr_t)0xFFFF

// VIRT_ADDR format: first 16 lsb are used for word address, the remaning bits are used to store segment id with 
//an offset(because addr 0 not allowed) get virtual address from a segment id
#define GET_VIRT_ADDR(seg_id) ((void *)((intptr_t)((VIRT_ADDR_OFFSET| seg_id) << 16)));
#define EXTRACT_SEG_ID_FROM_VIRT_ADDR(addr) ((int)(VIRT_ADDR_OFFSET ^ (intptr_t)((intptr_t)addr >> 16)));
#define EXTRACT_WORD_INDEX_FROM_VIRT_ADDR(addr, align) (((intptr_t)addr & LSB16) / align); // return id of word


//----------------------------------------------------------------------------------------------------------------------

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
