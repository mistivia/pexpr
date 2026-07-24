# API Guide

Everything lives in `include/pexpr.h`. This walks through it by use case;
see `doc/FORMAT.md` for the wire format itself.

## Values

```c
enum ptype { PTYPE_INTEG, PTYPE_REAL, PTYPE_STR, PTYPE_SYMBOL, PTYPE_LIST };

struct pnode {
    enum ptype type;
    union {
        int64_t integ;
        double real;
        struct { size_t str_len; const char *str; }; /* shared by PTYPE_STR and PTYPE_SYMBOL */
        struct { size_t list_len; size_t list_cap; struct pnode *list; }; /* array, by value */
    };
};
```

`PTYPE_SYMBOL` is a bareword matching `<initial> <subsequent>*`, or one of
the two peculiar identifiers `+`, `-` (e.g. `nil`, `set!`,
`foo-bar`, `list->vector`) - see `doc/FORMAT.md` for the full grammar.
There's no separate nil type, `nil` is just an ordinary symbol. Symbols
are case-insensitive: uppercase ASCII letters are folded to lowercase by
`pnode_make_symbol()`/`pnode_make_nsymbol()` and by the parser, so `"NIL"`
and `"nil"` are stored identically. `PTYPE_SYMBOL` is stored exactly like
`PTYPE_STR` (same `str`/`str_len` fields, built and dropped the same way);
only `type`, the case folding, and how it's serialized (no quotes, no
escaping) differ.

`struct pnode` is a plain value type: nothing in this library ever
allocates the struct itself, so you own its storage (a local variable, a
list's backing array, wherever) and this library only ever manages the
memory *inside* it - a string's bytes, or a list's child array.

`str` is always NUL-terminated in addition to tracking `str_len`
explicitly (the format allows embedded NULs, hence `str_len`). `list` is a
contiguous array of `list_cap` elements whose first `list_len` are valid
**values** (not pointers) owned directly by the node — walk it with
`for (size_t i = 0; i < node->list_len; i++)`, or use `pnode_list_len()`.
Because elements are values, `node->list[i]` is a
`struct pnode`; take its address (`&node->list[i]`) where a
`const struct pnode *` is expected, e.g. passing an element to
`pnode_list_len()` or `pexpr_serialize()`.

### Building values

```c
struct pnode nilsym = pnode_make_symbol("nil");
struct pnode n = pnode_make_integ(42);
struct pnode s = pnode_make_str("hi", 2);   /* copies the bytes */
struct pnode s2 = pnode_make_cstr("hi");    /* same, len = strlen(s) */

struct pnode list = pnode_make_list();       /* starts empty */
pnode_list_append(&list, pnode_make_integ(1));
pnode_list_append(&list, pnode_make_integ(2));
pnode_list_append(&list, s);                 /* moved into list; don't touch s again */

size_t n_children = pnode_list_len(&list);   /* 3 */
int64_t second = list.list[1].integ;         /* 2 */

pnode_drop(&list);                           /* releases everything list owns */
pnode_drop(&n);
pnode_drop(&s2);
pnode_drop(&nilsym);
```

`pnode_make_integ()`/`pnode_make_real()`/`pnode_make_list()` never fail
(they don't allocate anything up front). `pnode_make_str()`/
`pnode_make_cstr()`/`pnode_make_nsymbol()`/`pnode_make_symbol()` do
allocate a copy of the bytes immediately, so they *can* fail - check with
`pnode_ok()`:

```c
struct pnode s = pnode_make_str(huge_buf, huge_len);
if (!pnode_ok(&s)) {
    /* allocation failed; s.str is NULL, safe to pnode_drop() (a no-op)
     * or just drop on the floor */
}
```

`pnode_list_append()` **moves** `child`'s contents into the list's array
- on success, `child` is consumed: don't read it, drop it, or append it
again afterward (that's why the snippet above doesn't `pnode_drop(&s)`
separately). Since `child` is passed by value, a failed append (`-1`,
bad `list`, or allocation failure) simply leaves the caller's own copy
of `child` untouched, still theirs to drop.

### Dropping

```c
void pnode_drop(struct pnode *node);
```

`pnode_drop()` switches on `type` to release what a node owns directly
(the string buffer for `PTYPE_STR`, the `list` array itself for
`PTYPE_LIST`) and recurses into each element of `list` to release what
*it* owns in turn - but **not** `node` itself, since this library never
allocated it (nor, per below, does it ever hand back a heap pointer
anymore - there's nothing left in the API that isn't a plain value).
`pnode_drop()` is NULL-safe and safe to call more than once on the same
node (it resets what it just released to an empty, still-droppable
state).

### Copying

```c
struct pnode dup = pnode_copy(&list); /* deep copy, shares no memory with list */
pnode_drop(&list);                     /* dup is still fully valid */
pnode_drop(&dup);
```

`pnode_copy()` recursively duplicates a value — every string's bytes and
every list descendant get their own allocation — so the original and the
copy can be dropped independently in either order. `node` must not be
`NULL`. On allocation failure, returns a value for which `pnode_ok()` is
false; anything already copied before the failure is cleaned up
internally, so there's nothing extra for the caller to release in that
case.

## Serializing

```c
size_t len;
char *text = pexpr_serialize(&list, &len); /* NULL on error */
puts(text);
free(text);
```

`out_len` may be `NULL` if you don't need it (the buffer is NUL-terminated
regardless). Serialization fails (`NULL`) for a `NULL` node, a node for
which `pnode_ok()` is false anywhere in the tree, a NaN/Inf `double`
anywhere in the tree, a `PTYPE_SYMBOL` whose bytes don't match the symbol
grammar (see `doc/FORMAT.md`), or on allocation failure.

## Parsing a complete buffer

```c
struct pnode n = pexpr_parse("[1 2 [4 5 \"str\"]]", 17);
if (!pnode_ok(&n)) { /* syntax error, or allocation failure */ }
pnode_drop(&n);
```

Trailing bytes after the first complete value are ignored — this parses
exactly one value. Like every other constructor in this library,
`pexpr_parse()` returns `struct pnode` by value; check success with
`pnode_ok()` rather than a `NULL` check, and release the result (whether
or not it's ok - dropping a not-ok value is always a safe no-op) with
`pnode_drop()`.

## Streaming

Use the streaming parser when input arrives incrementally (a socket, a
file read loop, etc.) and you don't want to buffer the whole document
first.

```c
struct p_parser p;
p_parser_init(&p);

enum p_parser_state st;
do {
    ssize_t n = read(fd, chunk, sizeof chunk);
    if (n < 0) { /* handle error */ }
    st = p_parser_feed(&p, (size_t)n, chunk);
    if (n == 0) break; /* EOF: p_parser_feed(&p, 0, NULL) was just called */
} while (st == P_PARSER_PAUSE);

if (st == P_PARSER_SUCC) {
    struct pnode result = p_parser_get_result(&p);
    /* ... use result ... */
    pnode_drop(&result);
} else {
    fprintf(stderr, "parse error: %s\n", p_parser_errmsg(&p));
}

p_parser_reset(&p); /* ready for another document */
/* ... */
p_parser_destroy(&p);
```

Key points:

- **`len == 0` means end of input.** A bare top-level number like `42` has
  no closing delimiter, so the parser can't know it's complete until told
  there's nothing more (see `doc/FORMAT.md`). Values that *are*
  self-delimited (`]`, closing `"`) will reach `P_PARSER_SUCC` on their own
  without needing this.
- **Terminal states stick.** Once `p_parser_feed()` returns `P_PARSER_SUCC`
  or `P_PARSER_FAIL`, further calls return that same state without
  consuming input, until you call `p_parser_reset()`.
- **`p_parser_get_result()` does not reset the parser.** It just retrieves
  the value (once - a second call returns a not-ok value); `self` stays in
  `P_PARSER_SUCC` until you call `p_parser_reset()`, whether or not you
  called `get_result()` first.
- **`p_parser_reset()` is the one way to reuse `self`** after
  `P_PARSER_SUCC`/`P_PARSER_FAIL`, or to abandon an in-progress parse
  without destroying it — no need for `p_parser_destroy()` +
  `p_parser_init()` between documents. Returns 0, or -1 on allocation
  failure (leaves `self` in `P_PARSER_FAIL`, still safe to retry or
  destroy).
- **`p_parser_destroy()` is always safe**, including mid-parse. Both it and
  `p_parser_reset()` are built on top of a small stackless coroutine
  (`src/stackless.{c,h}`) so the recursive-descent parser can be written as
  ordinary, synchronous-looking code; abandoning mid-parse reclaims whatever
  partial value tree and
  scratch buffers were in flight (see the `open_lists` / `active_buf`
  comment in `src/parser.c`).
- `p_parser_errmsg()` is always safe to call and never returns `NULL`
  (empty string if there's nothing to report yet).

`pexpr_parse()` is a thin wrapper around exactly this loop for the common
case where you already have the whole buffer in memory.
