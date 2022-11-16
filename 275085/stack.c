#include "stack.h"
#include <stdlib.h>

int pop(stack_t *stack) {
  int index = -1;
  if (stack->top == -1) {
    return -1;
  }
  index = stack->vals[stack->top];
  stack->top -= 1;

  return index;
}

int push(stack_t *stack, int idx) {
  stack->top += 1;
  stack->vals[stack->top] = idx;
  return 0;
}