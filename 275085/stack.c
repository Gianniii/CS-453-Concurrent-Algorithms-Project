#include "stack.h"
#include <stdlib.h>

// TODO, IF DOESNT WORK TRY OTHER IMPLEMENTATATION

bool is_empty(stack_t *stack) { return stack->top == -1; }

bool is_full(stack_t *stack) { return stack->top == MAX_STACK_SIZE - 1; }

int pop(stack_t *stack) {
  int index = -1;
  if (is_empty(stack)) {
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