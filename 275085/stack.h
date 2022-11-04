

#define STACK_CAPACITY 0x10000 // 2^16

typedef struct{
    int* indices;
    int top;
} stack_t;