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
    PTYPE_SYMBOL,
    PTYPE_LIST,
};

/*
 * A pexpr value. `list` is a contiguous array of `list_len` valid child
 * values (not pointers), `list_cap` sized. PTYPE_SYMBOL shares the
 * `str`/`str_len` fields with PTYPE_STR - there's no separate nil type,
 * `nil` is just an ordinary symbol.
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
            size_t list_len;  /* number of valid elements */
            size_t list_cap;  /* allocated capacity of `list` */
            struct pnode *list; /* array of list_cap elements, by value; NULL iff list_cap == 0 */
        };
    };
};

/*
 * Construction. struct pnode is never heap-allocated by this library, so
 * these return it by value; the caller owns its storage.
 * pnode_make_integ()/real()/list() can't fail. pnode_make_str()/cstr()/
 * nsymbol()/symbol() copy `s` and can fail, signaled by `str == NULL` (see
 * pnode_ok()). Symbols are case-insensitive: uppercase ASCII letters are
 * folded to lowercase before storing; the symbol grammar itself (see
 * doc/FORMAT.md) isn't validated until pexpr_serialize(), same as
 * pnode_make_real() not rejecting NaN/Inf up front.
 */
struct pnode pnode_make_integ(int64_t v);
struct pnode pnode_make_real(double v);
struct pnode pnode_make_str(const char *s, size_t len); /* copies s */
struct pnode pnode_make_cstr(const char *s); /* copies s; len = strlen(s) */
struct pnode pnode_make_nsymbol(const char *s, size_t len); /* copies s */
struct pnode pnode_make_symbol(const char *s); /* copies s; len = strlen(s) */
struct pnode pnode_make_list(void); /* starts empty; cannot fail */

/* True if `node` is a successfully constructed value, as opposed to a
 * failure marker (see the constructors above, pnode_copy(),
 * pexpr_parse(), p_parser_get_result()). `node` must not be NULL. */
int pnode_ok(const struct pnode *node);

/* Moves `child` into `list` (must be PTYPE_LIST); don't touch `child`
 * again after a successful call. Returns 0 on success, -1 on error (bad
 * `list`, or allocation failure - `child` is left untouched, still the
 * caller's to drop). */
int pnode_list_append(struct pnode *list, struct pnode child);

/* Number of children in a PTYPE_LIST node. */
size_t pnode_list_len(const struct pnode *list);

/* Recursively frees what `node` owns, but not `node` itself (callers own
 * that storage, not this library). NULL-safe, idempotent. */
void pnode_drop(struct pnode *node);

/* Recursively deep-copies `node`; release the result separately with
 * pnode_drop(). `node` must not be NULL. On allocation failure, returns a
 * value for which pnode_ok() is false; nothing left to clean up. */
struct pnode pnode_copy(const struct pnode *node);

/* ------------------------------------------------------------------ *
 * Serialization (in-memory pnode -> P Expression text)
 * ------------------------------------------------------------------ */

/* Serializes `node` to newly malloc'd, NUL-terminated text (caller must
 * free() it); `out_len`, if non-NULL, receives the length excluding the
 * NUL. Returns NULL on error: NULL node, NaN/Inf double, a PTYPE_SYMBOL
 * not matching the symbol grammar (doc/FORMAT.md), or allocation failure. */
char *pexpr_serialize(const struct pnode *node, size_t *out_len);

/* ------------------------------------------------------------------ *
 * One-shot parsing
 * ------------------------------------------------------------------ */

/* Parses one complete value from `buf`; trailing bytes are ignored. On
 * syntax error, empty input, or allocation failure, returns a value for
 * which pnode_ok() is false. Release the result with pnode_drop(). */
struct pnode pexpr_parse(const char *buf, size_t len);

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

/* Feeds `len` bytes at `str`, running the parser until it completes
 * (P_PARSER_SUCC), hits a syntax error (P_PARSER_FAIL), or exhausts the
 * chunk and needs more (P_PARSER_PAUSE - feed the next chunk). `str` need
 * only stay valid for this call. A bare top-level number has no closing
 * delimiter, so call p_parser_feed(self, 0, NULL) to signal end of input.
 * Once P_PARSER_SUCC/P_PARSER_FAIL is reached, further feeds return that
 * same state without consuming input until p_parser_reset(). */
enum p_parser_state p_parser_feed(struct p_parser *self, size_t len, const char *str);

/* If in P_PARSER_SUCC and not already retrieved, returns the parsed value
 * (caller owns it, release with pnode_drop()); otherwise a value for
 * which pnode_ok() is false. Does not reset `self` - call
 * p_parser_reset() explicitly once done with the result. */
struct pnode p_parser_get_result(struct p_parser *self);

/* Human-readable description of the last error, valid after
 * P_PARSER_FAIL until the next reset/feed/destroy. Never NULL. */
const char *p_parser_errmsg(const struct p_parser *self);

/* Discards whatever `self` was doing - P_PARSER_SUCC (retrieved or not),
 * P_PARSER_FAIL, or an in-progress parse - and puts it back in
 * P_PARSER_PAUSE. The only way to reuse `self` after a terminal state, or
 * to abandon an in-progress parse without destroying it. Returns 0, or -1
 * on allocation failure (leaves `self` in P_PARSER_FAIL, `self->impl`
 * still valid - safe to retry or destroy). */
int p_parser_reset(struct p_parser *self);

/* Releases all resources held by `self`. Safe in any state, including
 * mid-parse. `self` needs a fresh p_parser_init() before reuse. */
void p_parser_destroy(struct p_parser *self);

#ifdef __cplusplus
}
#endif

#endif /* PEXPR_H */
