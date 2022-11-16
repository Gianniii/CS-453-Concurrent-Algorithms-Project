#pragma once

#include <stdbool.h>

#define MAX_STACK_SIZE 0x10000 // max number of words

typedef struct {
  int vals[MAX_STACK_SIZE];
  int top;
} stack_t;

int pop(stack_t *stack);
int push(stack_t *stack, int value);