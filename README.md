# pexpr

**AI-GENERATED CONTENT**

A small C library for **P Expression**, an S-expression-like serialization
format with five value types — nil, integers, reals, strings, and lists:

```
[1 2 [4 5 "str"]]
```

It provides an in-memory value type (`struct pnode`), a serializer, a
one-shot parser, and a streaming parser that can be fed input
incrementally (a byte, a chunk, a socket read at a time) and pauses
between calls instead of requiring the whole document up front.

See [`doc/FORMAT.md`](doc/FORMAT.md) for the format spec and
[`doc/API.md`](doc/API.md) for a full API walkthrough.

## Example

```c
#include "pexpr.h"
#include <stdio.h>

int main(void) {
    struct pnode list = pnode_make_list();
    pnode_list_append(&list, pnode_make_integ(1));
    pnode_list_append(&list, pnode_make_integ(2));
    pnode_list_append(&list, pnode_make_str("str", 3));

    char *text = pexpr_serialize(&list, NULL);
    puts(text);   /* [1 2 "str"] */
    free(text);
    pnode_drop(&list);

    struct pnode parsed = pexpr_parse("[1 2 [4 5 \"str\"]]", 17);
    printf("%zu top-level elements\n", pnode_list_len(&parsed)); /* 3 */
    pnode_drop(&parsed);

    return 0;
}
```

## Streaming

For input that arrives incrementally — a socket, a file read loop — the
streaming parser can be fed one chunk (or one byte) at a time instead of
requiring the whole document up front:

```c
#include "pexpr.h"
#include <stdio.h>

int main(void) {
    struct p_parser p;
    p_parser_init(&p);

    char chunk[4096];
    enum p_parser_state st;
    for (;;) {
        size_t n = fread(chunk, 1, sizeof chunk, stdin);
        st = p_parser_feed(&p, n, chunk);
        if (n == 0) break;              /* EOF: this feed() signaled it */
        if (st != P_PARSER_PAUSE) break; /* SUCC or FAIL already reached */
    }

    if (st == P_PARSER_SUCC) {
        struct pnode result = p_parser_get_result(&p);
        printf("%zu top-level elements\n", pnode_list_len(&result));
        pnode_drop(&result);
    } else {
        fprintf(stderr, "parse error: %s\n", p_parser_errmsg(&p));
    }

    p_parser_destroy(&p);
    return 0;
}
```

`p_parser_feed(&p, 0, ...)` (`n == 0` above) signals end of input — needed
because a bare top-level number like `42` has no closing delimiter to
tell the parser it's actually done. Values that are self-delimited (`]`,
a closing `"`) reach `P_PARSER_SUCC` on their own without it. See
[`doc/API.md`](doc/API.md#streaming) for the full walkthrough, including
reuse across documents and abandoning a parse mid-stream.

## Building

No external dependencies besides the vendored
[minicoro](https://github.com/edubart/minicoro) (`third_party/minicoro/`,
used to write the streaming parser as ordinary synchronous-looking code
instead of a hand-rolled state machine).

```sh
make            # build build/debug/libpexpr.a
make test       # build and run the unit tests
```

Link against `build/debug/libpexpr.a` with `-Iinclude`.

## Testing

```sh
make test       # plain debug build
make asan       # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan       # ThreadSanitizer
make msan       # MemorySanitizer (uninitialized reads; requires clang)
make coverage   # gcov/lcov line coverage report -> build/coverage/html/
make check      # all of the above
```

All four run the same test suite (`tests/`) against a differently
instrumented build; `make coverage` additionally writes an HTML coverage
report and prints a summary. As of this writing the suite passes clean
under all three sanitizers, with ~90% line coverage on `src/` (the
remainder is out-of-memory defensive branches, which would need a fault
injection harness to exercise — see `doc/FORMAT.md`'s design notes for
what's covered by test intent vs. left as documented gaps).

## Layout

```
include/pexpr.h        public API
src/                    implementation (pnode, serializer, streaming parser)
tests/                  unit tests (no external test framework)
doc/FORMAT.md           format specification
doc/API.md              API guide with examples
```