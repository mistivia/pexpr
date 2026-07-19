# API Guide

Everything lives in `include/pexpr.h`. This walks through it by use case;
see `doc/FORMAT.md` for the wire format itself.

## Values

```c
enum ptype { PTYPE_INTEG, PTYPE_REAL, PTYPE_STR, PTYPE_LIST };

struct pnode {
    enum ptype type;
    union {
        int64_t integ;
        double real;
        struct { size_t str_len; const char *str; };
        struct { size_t list_len; struct pnode *list; }; /* array, by value */
    };
};
```

`str` is always NUL-terminated in addition to tracking `str_len`
explicitly (the format allows embedded NULs, hence `str_len`). `list` is a
contiguous array of `list_len` **values** (not pointers) owned directly by
the node — walk it with `for (size_t i = 0; i < node->list_len; i++)`, or
use `pnode_list_len()`. Because elements are values, `node->list[i]` is a
`struct pnode`; take its address (`&node->list[i]`) where a
`const struct pnode *` is expected, e.g. passing an element to
`pnode_list_len()` or `pexpr_serialize()`.

`pnode_free()` switches on `type` to release what a node owns directly
(the string buffer for `PTYPE_STR`, the `list` array itself for
`PTYPE_LIST`) and recurses into each element of `list` to free what *it*
owns in turn. Scalars (`PTYPE_INTEG` / `PTYPE_REAL`) own nothing extra.

### Building values

```c
struct pnode *n = pnode_new_integ(42);
struct pnode *s = pnode_new_str("hi", 2);        /* copies the bytes */
struct pnode *s2 = pnode_new_cstr("hi");         /* same, len = strlen(s) */

struct pnode *list = pnode_new_list();            /* starts empty */
pnode_list_append(list, pnode_new_integ(1));
pnode_list_append(list, pnode_new_integ(2));
pnode_list_append(list, s);                        /* moved into list; don't touch s again */

size_t n_children = pnode_list_len(list);          /* 3 */
int64_t second = list->list[1].integ;              /* 2 */

pnode_free(list);                                   /* frees everything */
pnode_free(n);
pnode_free(s2);
```

`pnode_list_append()` **moves** `child`'s contents into the list's array
and frees `child`'s now-empty shell — on success, `child` is consumed:
don't read it, free it, or append it again afterward (that's why the
snippet above doesn't `pnode_free(s)`). On failure (`-1`) `child` is left
completely untouched and is still yours to free, same as before.

### Copying

```c
struct pnode *dup = pnode_copy(list); /* deep copy, shares no memory with list */
pnode_free(list);                      /* dup is still fully valid */
pnode_free(dup);
```

`pnode_copy()` recursively duplicates a value — every string's bytes and
every list descendant get their own allocation — so the original and the
copy can be freed independently in either order. Returns `NULL` for a
`NULL` input or on allocation failure.

## Serializing

```c
size_t len;
char *text = pexpr_serialize(list, &len); /* NULL on error */
puts(text);
free(text);
```

`out_len` may be `NULL` if you don't need it (the buffer is NUL-terminated
regardless). Serialization fails (`NULL`) for a `NULL` node, a NaN/Inf
`double` anywhere in the tree, or on allocation failure.

## Parsing a complete buffer

```c
struct pnode *n = pexpr_parse("[1 2 [4 5 \"str\"]]", 17);
if (!n) { /* syntax error, or allocation failure */ }
pnode_free(n);
```

Trailing bytes after the first complete value are ignored — this parses
exactly one value.

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
    struct pnode *result = p_parser_get_result(&p); /* also resets p */
    /* ... use result ... */
    pnode_free(result);
} else {
    fprintf(stderr, "parse error: %s\n", p_parser_errmsg(&p));
}

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
  consuming input, until you call `p_parser_get_result()` (on success) or
  reinitialize.
- **`p_parser_get_result()` resets the parser** on success, so the same
  `struct p_parser` can parse another value right away — no need to
  `p_parser_destroy()` + `p_parser_init()` between documents.
- **`p_parser_destroy()` is always safe**, including mid-parse (e.g. you
  decide to give up after a timeout). It's implemented on top of a
  [minicoro](https://github.com/edubart/minicoro) coroutine so the
  recursive-descent parser can be written as ordinary, synchronous-looking
  code; destroying mid-parse reclaims whatever partial value tree and
  scratch buffers were in flight at the point it paused (see the
  `open_lists` / `active_buf` comment in `src/parser.c` if you're curious
  why that needs explicit bookkeeping instead of just freeing the
  coroutine stack).
- `p_parser_errmsg()` is always safe to call and never returns `NULL`
  (empty string if there's nothing to report yet).

`pexpr_parse()` is a thin wrapper around exactly this loop for the common
case where you already have the whole buffer in memory.
