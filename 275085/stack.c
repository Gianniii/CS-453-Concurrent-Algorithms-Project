#include "stack.h"
#include <stdbool.h>
#include <stdlib.h>

bool is_empty(stack_t* stack) {
    return stack->top == -1;
}

bool is_full(stack_t* stack) {
    return stack->top == STACK_CAPACITY/2 - 1;
}

int pop(stack_t* stack) {
    int index = -1;
    if(!is_empty(stack)) {
        index = stack->indices[stack->top];
        stack->top -= 1;
    }
    return index;
}

int push(stack_t* stack, int idx) {
    if(is_full(stack)) {
        if(realloc(stack->indices, (2 * stack->top) * sizeof(int*)) == NULL) {
            return 1;
        }
    }
    stack->top += 1;
    stack->indices[stack->top] = idx;
    return 0;
}