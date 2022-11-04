#pragma once

#include <stdbool.h>

#define STACK_CAPACITY 0x10000 // 2^16

typedef struct{
    int* indices;
    int top;
} stack_t;

bool is_empty(stack_t* stack);
bool is_full(stack_t* stack);
int pop(stack_t* stack);
int push(stack_t* stack, int idx);