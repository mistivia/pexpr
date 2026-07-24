/*
 * Stackless coroutine primitives (Duff's-device style).
 *
 * A coroutine is an ordinary C function whose body is wrapped in
 * CO_BEGIN(co)/CO_END(co). It runs to the next CO_YIELD (suspend, resumed
 * on the next call) or CO_DONE/CO_END (finished), returning enum co_state
 * each time. Recursion is provided explicitly: CO_CALL invokes a
 * sub-"procedure" (a labelled case block declared with CO_PROC_ID) and
 * CO_RET returns from it, with per-frame locals kept on a heap-allocated
 * stack (co_push/co_top/co_pop) reachable through CO_VAR.
 *
 * Constraints on a coroutine body:
 *   - CO_YIELD expands to `return`, and CO_CALL/CO_RET re-dispatch with a
 *     `goto` to the CO_BEGIN switch (not `continue`), so all three are safe
 *     inside nested C loops -- ordinary `for`/`while` loops may freely
 *     contain calls and yields.
 *   - Do not use your own `switch` in the body: it would capture the
 *     machinery's `case` labels. `break`/`continue` are fine, but only bind
 *     to a loop you wrote yourself; at the top level of a procedure they
 *     would hit the dispatch switch, so exit a procedure with CO_RET, not
 *     `break`.
 *   - Resuming jumps straight to a CO_YIELD/CO_CALL label, skipping any
 *     local variable initialization above it, so state that must survive a
 *     suspension belongs in the frame (CO_VAR) or in the coroutine's owner
 *     struct, not in a plain local.
 *
 * Adapted from https://gist.github.com/mistivia/be432897e9be2782f83315c0871320e5
 */
#pragma once

#include <stdlib.h>

enum co_state {
    CO_PAUSED,
    CO_FINISHED
};

struct co_stack_frame {
    int ret_addr;
    void *vars;
};

struct co {
    int pc;
    int sp;
    int stksz;
    enum co_state state;
    struct co_stack_frame *stack;
};

void co_init(struct co *self);
void co_drop(struct co *self);
void co_push(struct co *self, int ret_addr, const void *vars, size_t size);
/* Like co_push() but with no `vars` frame (used by CO_CALL0 for procedures
 * that take no arguments); such a frame must not be read with co_top(). */
void co_push_noargs(struct co *self, int ret_addr);
int co_pop(struct co *self);
void *co_top(struct co *self);

#define CO_PROC_ID(_name) enum { _name = __COUNTER__ };

/* The single dispatch point every CO_CALL/CO_RET jumps back to; `co_dispatch`
 * lives in the label namespace, so it never collides with other identifiers. */
#define CO_BEGIN(_co) co_dispatch: switch ((_co)->pc) { case -100:

#define CO_END(_co) } (_co)->state = CO_FINISHED; return CO_FINISHED;

#define CO_YIELD(_co) CO_YIELD_((_co), __COUNTER__)
#define CO_YIELD_(_co, _id) \
    do { \
        (_co)->pc = _id; \
        (_co)->state = CO_PAUSED; \
        return CO_PAUSED; \
        case _id: ; \
    } while (0)

#define CO_DONE(_co) \
    do { \
        (_co)->state = CO_FINISHED; \
        return CO_FINISHED; \
    } while (0)

#define CO_CALL(_co, _proc, _vars) CO_CALL_((_co), (_proc), _vars, __COUNTER__)
#define CO_CALL_(_co, _proc, _vars, _id) \
    do { \
        co_push((_co), _id, &(_vars), sizeof(_vars)); \
        (_co)->pc = (_proc); \
        goto co_dispatch; \
        case _id: ; \
    } while (0)

/* Invoke a procedure that takes no arguments (no frame `vars`); it must not
 * use CO_VAR. Everything else matches CO_CALL. */
#define CO_CALL0(_co, _proc) CO_CALL0_((_co), (_proc), __COUNTER__)
#define CO_CALL0_(_co, _proc, _id) \
    do { \
        co_push_noargs((_co), _id); \
        (_co)->pc = (_proc); \
        goto co_dispatch; \
        case _id: ; \
    } while (0)

#define CO_RET(_co) \
    do { \
        (_co)->pc = co_pop(_co); \
        goto co_dispatch; \
    } while (0)

#define CO_VAR(_co, _type, _name) (((_type*)co_top(_co))->_name)
