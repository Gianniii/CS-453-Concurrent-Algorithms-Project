#pragma once

#include <stdbool.h>

#define MAX_STACK_SIZE 0x10000 // max number of words

typedef struct {
  int *vals;
  int top;
} stack_t;

bool is_empty(stack_t *stack);
bool is_full(stack_t *stack);
int pop(stack_t *stack);
int push(stack_t *stack, int value);