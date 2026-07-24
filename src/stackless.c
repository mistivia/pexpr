#include "stackless.h"

#include <string.h>

void co_init(struct co *self) {
    self->pc = -100;
    self->sp = 0;
    self->stksz = 16;
    self->state = CO_PAUSED;
    self->stack = malloc(self->stksz * sizeof(struct co_stack_frame));
}

void co_drop(struct co *self) {
    for (int i = 0; i < self->sp; i++) {
        free(self->stack[i].vars);
    }
    free(self->stack);
    self->stack = NULL;
    self->sp = 0;
    self->stksz = 0;
}

void co_push(struct co *self, int ret_addr, const void *vars, size_t size) {
    if (self->sp == self->stksz) {
        self->stksz *= 2;
        self->stack = realloc(self->stack, self->stksz * sizeof(struct co_stack_frame));
    }
    struct co_stack_frame *frame = &self->stack[self->sp++];
    frame->ret_addr = ret_addr;
    frame->vars = malloc(size);
    memcpy(frame->vars, vars, size);
}

void co_push_noargs(struct co *self, int ret_addr) {
    if (self->sp == self->stksz) {
        self->stksz *= 2;
        self->stack = realloc(self->stack, self->stksz * sizeof(struct co_stack_frame));
    }
    struct co_stack_frame *frame = &self->stack[self->sp++];
    frame->ret_addr = ret_addr;
    frame->vars = NULL; /* co_pop's free(NULL) is a no-op */
}

int co_pop(struct co *self) {
    struct co_stack_frame *frame = &self->stack[--self->sp];
    int ret_addr = frame->ret_addr;
    free(frame->vars);
    frame->vars = NULL;
    return ret_addr;
}

void *co_top(struct co *self) {
    if (self->sp == 0) {
        return NULL;
    }
    return self->stack[self->sp - 1].vars;
}
