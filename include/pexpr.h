/*
 * pexpr - a small serialization format ("P Expression") and its C library.
 *
 * See doc/FORMAT.md for the on-wire format and doc/API.md for a full API
 * walk-through with examples. This header only documents behavior that
 * isn't obvious from the signature.
 */
#ifndef PEXPR_H
#define PEXPR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Value model
 * ------------------------------------------------------------------ */

enum ptype {
    PTYPE_INTEG,
    PTYPE_REAL,
    PTYPE_STR,
    PTYPE_LIST,
};

/*
 * A pexpr value. `list` is a contiguous array of `list_len` child values
 * (not pointers) owned directly by this node; pnode_free() switches on
 * `type` to release what a node owns directly (the string buffer, or the
 * `list` array itself) and recurses into each element of `list` to
 * release its owned memory in turn.
 */
struct pnode {
    enum ptype type;
    union {
        int64_t integ;
        double real;
        struct {
            size_t str_len;
            const char *str; /* always NUL-terminated in addition to str_len */
        };
        struct {
            size_t list_len;
            struct pnode *list; /* array of list_len elements, by value; NULL iff list_len == 0 */
        };
    };
};

/*
 * Construction. These return the value directly rather than a pointer:
 * PTYPE_INTEG/PTYPE_REAL never allocate, so they can't fail.
 * PTYPE_STR/PTYPE_LIST do own heap memory (a string buffer, or a child
 * array once non-empty), but nothing is allocated up front - only
 * pnode_new_str()/pnode_new_cstr() can fail (a copy of `s` is made
 * immediately), signaled by returning a node with `str == NULL` (never
 * true on success, even for an empty string - see pnode_ok()).
 */
struct pnode pnode_new_integ(int64_t v);
struct pnode pnode_new_real(double v);
struct pnode pnode_new_str(const char *s, size_t len); /* copies s */
struct pnode pnode_new_cstr(const char *s); /* copies s; len = strlen(s) */
struct pnode pnode_new_list(void); /* starts empty; cannot fail */

/*
 * True if `node` is a successfully constructed value, as opposed to the
 * failure marker pnode_new_str()/pnode_new_cstr()/pnode_copy() return on
 * allocation failure. Always true for PTYPE_INTEG/PTYPE_REAL and for a
 * genuinely empty PTYPE_LIST. `node` must not be NULL.
 */
int pnode_ok(const struct pnode *node);

/*
 * Appends `child` to `list` (must be PTYPE_LIST) by moving its contents
 * into `list`'s array; `child` is consumed on success - the caller's own
 * copy must not be freed or read afterward (its payload, if any, now
 * belongs to `list`). Returns 0 on success, -1 on error (bad `list`, or
 * allocation failure - on failure `child` is completely untouched, since
 * it was passed by value, and remains the caller's to free).
 */
int pnode_list_append(struct pnode *list, struct pnode child);

/* Number of children in a PTYPE_LIST node. */
size_t pnode_list_len(const struct pnode *list);

/* Recursively frees everything `node` owns (a string buffer, or a list's
 * elements and their own owned memory), but not `node` itself - callers
 * own the storage for `struct pnode` values now (stack, array element,
 * etc.), not this library. NULL-safe. Safe to call more than once: it
 * resets what it just freed to an empty, still-freeable state. */
void pnode_free(struct pnode *node);

/* Recursively deep-copies a node (including every list descendant and
 * every string's bytes); the result shares no memory with `node` and
 * must be freed separately with pnode_free(). `node` must not be NULL.
 * On allocation failure, returns a value for which pnode_ok() is false;
 * whatever was already copied before the failure is freed internally, so
 * there is nothing for the caller to clean up in that case. */
struct pnode pnode_copy(const struct pnode *node);

/* Frees a heap-allocated struct pnode* such as pexpr_parse() or
 * p_parser_get_result() returns - equivalent to pnode_free(node)
 * followed by free(node). NULL-safe. Don't use this on a struct pnode
 * you hold by value (a local variable, a pnode_new_*() result, a list
 * element): use plain pnode_free() on its address instead, since it was
 * never itself heap-allocated by this library. */
void pnode_delete(struct pnode *node);

/* ------------------------------------------------------------------ *
 * Serialization (in-memory pnode -> P Expression text)
 * ------------------------------------------------------------------ */

/*
 * Serializes `node` to newly malloc'd, NUL-terminated text. If `out_len`
 * is non-NULL, receives the length of the text excluding the NUL.
 * Returns NULL on error (NULL node, non-finite double, or allocation
 * failure) - non-finite doubles (NaN/Inf) have no decimal representation
 * per the format, so encoding one is an error rather than best-effort.
 * Caller owns the returned buffer and must free() it.
 */
char *pexpr_serialize(const struct pnode *node, size_t *out_len);

/* ------------------------------------------------------------------ *
 * One-shot parsing
 * ------------------------------------------------------------------ */

/* Parses one complete P Expression value from `buf`. Returns NULL on
 * syntax error, empty input, or allocation failure; otherwise a
 * heap-allocated node the caller owns - free it with pnode_delete().
 * Trailing bytes after the value (if any) are ignored. */
struct pnode *pexpr_parse(const char *buf, size_t len);

/* ------------------------------------------------------------------ *
 * Streaming parser
 * ------------------------------------------------------------------ */

enum p_parser_state {
    P_PARSER_FAIL,
    P_PARSER_SUCC,
    P_PARSER_PAUSE,
};

struct p_parser_impl; /* opaque */

struct p_parser {
    struct p_parser_impl *impl;
};

/* Initializes `self` (caller-allocated, e.g. on the stack). Returns 0 on
 * success, -1 on allocation failure. */
int p_parser_init(struct p_parser *self);

/*
 * Feeds `len` bytes at `str` to the parser and runs it until it either
 * completes a value (P_PARSER_SUCC), hits a syntax error
 * (P_PARSER_FAIL), or exhausts the given bytes and needs more
 * (P_PARSER_PAUSE) - call p_parser_feed() again with the next chunk in
 * that case. `str` need only stay valid for the duration of this call.
 *
 * End of input: since a bare top-level number has no closing delimiter,
 * the parser cannot know it is complete until told there is nothing
 * more. Call p_parser_feed(self, 0, NULL) (len == 0) to signal end of
 * input; this is the one addition this implementation makes on top of
 * DESIGN's literal signature, and is needed for correctness.
 *
 * Once the parser has reached P_PARSER_SUCC or P_PARSER_FAIL, further
 * feed() calls return that same terminal state without consuming input,
 * until p_parser_get_result() (on success) or p_parser_init()/
 * p_parser_destroy() (on failure) resets things.
 */
enum p_parser_state p_parser_feed(struct p_parser *self, size_t len, const char *str);

/*
 * If the parser is in P_PARSER_SUCC, returns the parsed node (caller
 * owns it, free with pnode_delete()) and resets `self` so it is ready to
 * parse another value. Otherwise returns NULL and leaves `self`
 * untouched.
 */
struct pnode *p_parser_get_result(struct p_parser *self);

/* Human-readable description of the last error, valid after
 * P_PARSER_FAIL until the next init/feed/destroy. Never NULL. */
const char *p_parser_errmsg(const struct p_parser *self);

/* Releases all resources held by `self` (including its coroutine stack).
 * Safe to call in any state, including mid-parse if you're abandoning a
 * parse early. `self` must not be used again without a fresh
 * p_parser_init(). */
void p_parser_destroy(struct p_parser *self);

#ifdef __cplusplus
}
#endif

#endif /* PEXPR_H */
