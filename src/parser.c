#include "pexpr.h"
#include "pbuf.h"
#include "minicoro.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum list nesting depth. Guards the coroutine's fixed-size stack
 * against unbounded recursion on adversarial input. */
#define PEXPR_MAX_DEPTH 256

struct p_parser_impl {
    mco_coro *co;
    enum p_parser_state state;

    /* Current chunk being fed; only valid while inside mco_resume(). */
    const char *in_buf;
    size_t in_len;
    size_t in_pos;
    int eof;

    /* Set by the coroutine entry point when it finishes; handed back by
     * value from p_parser_get_result(). */
    int parse_ok;
    struct pnode result;

    /*
     * If p_parser_destroy() is called while the coroutine is suspended
     * mid-parse, minicoro just deallocates its stack - any C local
     * variables on it (partially built pnode trees, in-progress string/
     * number scratch buffers) are never unwound, so whatever they own
     * would otherwise leak. These two fields let the parser register its
     * in-flight allocations so destroy() can still free them:
     *
     * - `open_lists` holds the address of every PTYPE_LIST value
     *   currently under construction (one push per active parse_list()
     *   stack frame, popped right before that frame returns via any
     *   path). These addresses point *into the coroutine's own stack*
     *   (a local variable in a suspended call frame) - valid to read
     *   right up until mco_destroy() deallocates that stack, which is
     *   why reclaim_in_flight() must run before it. A node only becomes
     *   reachable from its parent's `list` array *after* its own
     *   parse_list() call returns, so at any given yield point these
     *   nodes are not yet linked to each other and pnode_drop() (which
     *   only ever touches what a node *owns*, never the node's own
     *   storage) can be called on each independently.
     * - `active_buf` points at the single struct pbuf (if any) currently
     *   accumulating a string or number token - also coroutine-stack
     *   memory. Only one can be active at a time, since collecting a
     *   token never recurses.
     */
    struct pnode **open_lists;
    size_t open_lists_len;
    size_t open_lists_cap;
    struct pbuf *active_buf;

    char errmsg[160];
};

/* Canonical "no value" marker for the parser's two boundary functions,
 * distinguishable from every successful parse via pnode_ok(): a
 * PTYPE_LIST with list_len != 0 but list == NULL never occurs otherwise
 * (see pnode_copy()'s use of the same convention in src/pnode.c). */
static struct pnode invalid_result(void) {
    struct pnode n;
    n.type = PTYPE_LIST;
    n.list = NULL;
    n.list_len = (size_t)-1;
    n.list_cap = 0;
    return n;
}

static int push_open_list(struct p_parser_impl *p, struct pnode *list) {
    if (p->open_lists_len == p->open_lists_cap) {
        size_t new_cap = p->open_lists_cap ? p->open_lists_cap * 2 : 8;
        struct pnode **new_arr = realloc(p->open_lists, new_cap * sizeof *new_arr);
        if (!new_arr) return -1;
        p->open_lists = new_arr;
        p->open_lists_cap = new_cap;
    }
    p->open_lists[p->open_lists_len++] = list;
    return 0;
}

static void pop_open_list(struct p_parser_impl *p) {
    p->open_lists_len--;
}

/* Frees whatever the parser currently has in flight. Safe to call
 * regardless of parser state: after a normal SUCC/FAIL completion these
 * are already empty, since every code path pops/clears them before
 * returning. */
static void reclaim_in_flight(struct p_parser_impl *p) {
    if (p->active_buf) {
        pbuf_free(p->active_buf);
        p->active_buf = NULL;
    }
    for (size_t i = p->open_lists_len; i-- > 0;) {
        pnode_drop(p->open_lists[i]);
    }
    free(p->open_lists);
    p->open_lists = NULL;
    p->open_lists_len = 0;
    p->open_lists_cap = 0;
}

struct parse_ctx {
    struct p_parser_impl *p;
    int depth;
};

/* ------------------------------------------------------------------ *
 * Byte source: reads from the chunk currently installed by feed(),
 * yielding back to p_parser_feed() whenever it runs out and the caller
 * hasn't signaled end of input yet.
 * ------------------------------------------------------------------ */

/* Returns 1 and sets *out without consuming it, or returns 0 once input
 * is permanently exhausted (EOF signaled by the caller). */
static int pk_peek(struct p_parser_impl *p, unsigned char *out) {
    for (;;) {
        if (p->in_pos < p->in_len) {
            *out = (unsigned char)p->in_buf[p->in_pos];
            return 1;
        }
        if (p->eof) return 0;
        mco_yield(mco_running());
    }
}

static void pk_advance(struct p_parser_impl *p) {
    p->in_pos++;
}

static int pk_getc(struct p_parser_impl *p, unsigned char *out) {
    if (!pk_peek(p, out)) return 0;
    pk_advance(p);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Recursive-descent grammar
 * ------------------------------------------------------------------ */

static int fail(struct p_parser_impl *p, const char *msg) {
    snprintf(p->errmsg, sizeof p->errmsg, "%s", msg);
    return 0;
}

static int is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(struct p_parser_impl *p) {
    unsigned char c;
    while (pk_peek(p, &c) && is_ws(c)) {
        pk_advance(p);
    }
}

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Every parse_*() function below fills `*out` (caller-owned storage,
 * typically a local variable somewhere on the coroutine's stack) and
 * returns 1 on success, or returns 0 and leaves `*out` untouched on
 * failure. */
static int parse_value(struct parse_ctx *ctx, struct pnode *out);

/* Clears the active-scratch-buffer registration, frees it, and fails.
 * Centralizes the bookkeeping every error exit from parse_string() and
 * parse_number() needs (see `active_buf` in struct p_parser_impl). */
static int buf_fail(struct p_parser_impl *p, struct pbuf *buf, const char *msg) {
    p->active_buf = NULL;
    pbuf_free(buf);
    return fail(p, msg);
}

static int parse_string(struct p_parser_impl *p, struct pnode *out) {
    unsigned char c = {0};
    if (!pk_getc(p, &c) || c != '"') return fail(p, "expected '\"'");

    struct pbuf buf = {0};
    pbuf_init(&buf);
    p->active_buf = &buf;

    for (;;) {
        if (!pk_getc(p, &c)) {
            return buf_fail(p, &buf, "unterminated string");
        }
        if (c == '"') break;

        unsigned char val;
        if (c == '\\') {
            unsigned char e;
            if (!pk_getc(p, &e)) {
                return buf_fail(p, &buf, "unterminated escape sequence");
            }
            switch (e) {
                case '0': val = '\0'; break;
                case '\\': val = '\\'; break;
                case 'a': val = '\a'; break;
                case 'b': val = '\b'; break;
                case 't': val = '\t'; break;
                case 'n': val = '\n'; break;
                case 'v': val = '\v'; break;
                case 'f': val = '\f'; break;
                case 'r': val = '\r'; break;
                case '"': val = '"'; break;
                case 'x': {
                    unsigned char h1, h2;
                    int hi, lo;
                    if (!pk_getc(p, &h1) || (hi = hex_val(h1)) < 0 ||
                        !pk_getc(p, &h2) || (lo = hex_val(h2)) < 0) {
                        return buf_fail(p, &buf, "invalid \\x escape");
                    }
                    val = (unsigned char)((hi << 4) | lo);
                    break;
                }
                default:
                    return buf_fail(p, &buf, "unknown escape sequence");
            }
        } else {
            val = c;
        }

        if (pbuf_putc(&buf, (char)val) != 0) {
            return buf_fail(p, &buf, "out of memory");
        }
    }

    size_t len;
    char *s = pbuf_release(&buf, &len);
    p->active_buf = NULL;
    if (!s) return fail(p, "out of memory");

    struct pnode node = pnode_make_str(s, len);
    free(s);
    if (!pnode_ok(&node)) return fail(p, "out of memory");

    *out = node;
    return 1;
}

static int is_num_char(unsigned char c) {
    return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
           c == '+' || c == '-';
}

static int parse_number(struct p_parser_impl *p, struct pnode *out) {
    struct pbuf buf = {0};
    pbuf_init(&buf);
    p->active_buf = &buf;

    unsigned char c;
    while (pk_peek(p, &c) && is_num_char(c)) {
        if (pbuf_putc(&buf, (char)c) != 0) {
            return buf_fail(p, &buf, "out of memory");
        }
        pk_advance(p);
    }

    size_t len;
    char *tok = pbuf_release(&buf, &len);
    p->active_buf = NULL;
    if (!tok) return fail(p, "out of memory");

    /* len is never 0 here: parse_value() only calls parse_number() after
     * peeking a byte that already satisfies is_num_char(). */

    int is_float = 0;
    for (size_t i = 0; i < len; i++) {
        if (tok[i] == '.' || tok[i] == 'e' || tok[i] == 'E') {
            is_float = 1;
            break;
        }
    }

    char *end = NULL;
    errno = 0;
    if (is_float) {
        double v = strtod(tok, &end);
        if (end != tok + len || errno == ERANGE) {
            free(tok);
            return fail(p, "invalid number literal");
        }
        *out = pnode_make_real(v);
    } else {
        long long v = strtoll(tok, &end, 10);
        if (end != tok + len || errno == ERANGE) {
            free(tok);
            return fail(p, "invalid number literal");
        }
        *out = pnode_make_integ((int64_t)v);
    }
    free(tok);
    return 1; /* pnode_make_integ()/pnode_make_real() never fail */
}

static int is_letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_special_initial(unsigned char c) {
    switch (c) {
        case '!': case '$': case '%': case '&': case '*': case '/':
        case ':': case '<': case '=': case '>': case '?': case '~':
        case '_': case '^':
            return 1;
        default:
            return 0;
    }
}

/* <initial> := <letter> | ! | $ | % | & | * | / | : | < | = | > | ? | ~ | _ | ^ */
static int is_symbol_initial(unsigned char c) {
    return is_letter(c) || is_special_initial(c);
}

/* <subsequent> := <initial> | <digit> | . | + | - | @ */
static int is_symbol_subsequent(unsigned char c) {
    return is_symbol_initial(c) || (c >= '0' && c <= '9') ||
           c == '.' || c == '+' || c == '-' || c == '@';
}

/* Reads a token via `pbuf`, using `is_char` to decide which bytes belong to
 * it. Shared by parse_symbol() and parse_number_or_symbol() - both just
 * greedily collect bytes and classify the result afterward. Returns NULL
 * on allocation failure (either mid-collection or at release); the caller
 * reports that uniformly, same as every other token reader in this file. */
static char *collect_token(struct p_parser_impl *p, int (*is_char)(unsigned char), size_t *out_len) {
    struct pbuf buf = {0};
    pbuf_init(&buf);
    p->active_buf = &buf;

    unsigned char c;
    while (pk_peek(p, &c) && is_char(c)) {
        if (pbuf_putc(&buf, (char)c) != 0) {
            p->active_buf = NULL;
            pbuf_free(&buf);
            return NULL;
        }
        pk_advance(p);
    }

    char *tok = pbuf_release(&buf, out_len);
    p->active_buf = NULL;
    return tok;
}

/* Dispatched when the first byte is a letter or a <special initial> byte -
 * unambiguously the start of <initial> <subsequent>*, never a number. */
static int parse_symbol(struct p_parser_impl *p, struct pnode *out) {
    size_t len;
    char *tok = collect_token(p, is_symbol_subsequent, &len);
    if (!tok) return fail(p, "out of memory");

    /* len is never 0 here: parse_value() only calls parse_symbol() after
     * peeking a byte that already satisfies is_symbol_initial(), a subset
     * of is_symbol_subsequent(). */

    struct pnode node = pnode_make_nsymbol(tok, len);
    free(tok);
    if (!pnode_ok(&node)) return fail(p, "out of memory");

    *out = node;
    return 1;
}

static int is_peculiar_symbol(const char *tok, size_t len) {
    return len == 1 && (tok[0] == '+' || tok[0] == '-');
}

/* Dispatched when the first byte is '+' or '-' - these are shared between
 * numbers (leading sign) and the two "peculiar identifier" symbols `+`
 * and `-`, so the token has to be collected first and classified
 * afterward rather than picked apart char-by-char. */
static int parse_number_or_symbol(struct p_parser_impl *p, struct pnode *out) {
    size_t len;
    char *tok = collect_token(p, is_symbol_subsequent, &len);
    if (!tok) return fail(p, "out of memory");

    /* len is never 0 here: parse_value() only calls this after peeking a
     * byte that already satisfies is_symbol_subsequent() ('+' and '-'
     * are both in that set). */

    if (is_peculiar_symbol(tok, len)) {
        struct pnode node = pnode_make_nsymbol(tok, len);
        free(tok);
        if (!pnode_ok(&node)) return fail(p, "out of memory");
        *out = node;
        return 1;
    }

    for (size_t i = 0; i < len; i++) {
        if (!is_num_char((unsigned char)tok[i])) {
            free(tok);
            return fail(p, "invalid number literal");
        }
    }

    int is_float = 0;
    for (size_t i = 0; i < len; i++) {
        if (tok[i] == '.' || tok[i] == 'e' || tok[i] == 'E') {
            is_float = 1;
            break;
        }
    }

    char *end = NULL;
    errno = 0;
    if (is_float) {
        double v = strtod(tok, &end);
        if (end != tok + len || errno == ERANGE) {
            free(tok);
            return fail(p, "invalid number literal");
        }
        *out = pnode_make_real(v);
    } else {
        long long v = strtoll(tok, &end, 10);
        if (end != tok + len || errno == ERANGE) {
            free(tok);
            return fail(p, "invalid number literal");
        }
        *out = pnode_make_integ((int64_t)v);
    }
    free(tok);
    return 1;
}

static int parse_list(struct parse_ctx *ctx, struct pnode *out) {
    struct p_parser_impl *p = ctx->p;
    unsigned char c = {0};
    if (!pk_getc(p, &c) || c != '[') return fail(p, "expected '['");

    ctx->depth++;
    if (ctx->depth > PEXPR_MAX_DEPTH) {
        ctx->depth--;
        return fail(p, "list nesting too deep");
    }

    struct pnode list = pnode_make_list(); /* cannot fail */
    if (push_open_list(p, &list) != 0) {
        ctx->depth--;
        return fail(p, "out of memory");
    }

    for (;;) {
        skip_ws(p);
        if (!pk_peek(p, &c)) {
            pop_open_list(p);
            pnode_drop(&list);
            ctx->depth--;
            return fail(p, "unterminated list");
        }
        if (c == ']') {
            pk_advance(p);
            break;
        }

        struct pnode child;
        if (!parse_value(ctx, &child)) {
            pop_open_list(p);
            pnode_drop(&list);
            ctx->depth--;
            return 0;
        }
        if (pnode_list_append(&list, child) != 0) {
            pnode_drop(&child);
            pop_open_list(p);
            pnode_drop(&list);
            ctx->depth--;
            return fail(p, "out of memory");
        }
    }

    pop_open_list(p);
    ctx->depth--;
    *out = list;
    return 1;
}

static int parse_value(struct parse_ctx *ctx, struct pnode *out) {
    struct p_parser_impl *p = ctx->p;
    skip_ws(p);

    unsigned char c;
    if (!pk_peek(p, &c)) return fail(p, "unexpected end of input");

    if (c == '[') return parse_list(ctx, out);
    if (c == '"') return parse_string(p, out);
    if (is_symbol_initial(c)) return parse_symbol(p, out);
    if (c == '+' || c == '-') return parse_number_or_symbol(p, out);
    if (c == '.' || (c >= '0' && c <= '9')) return parse_number(p, out);
    return fail(p, "unexpected character");
}

static void parser_entry(mco_coro *co) {
    struct p_parser_impl *p = (struct p_parser_impl *)mco_get_user_data(co);
    struct parse_ctx ctx = {0};
    ctx.p = p;
    ctx.depth = 0;

    p->parse_ok = parse_value(&ctx, &p->result);
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

static mco_result start_coroutine(struct p_parser_impl *p) {
    mco_desc desc = mco_desc_init(parser_entry, 0);
    desc.user_data = p;
    return mco_create(&p->co, &desc);
}

int p_parser_init(struct p_parser *self) {
    if (!self) return -1;

    struct p_parser_impl *p = calloc(1, sizeof *p);
    if (!p) {
        self->impl = NULL;
        return -1;
    }

    if (start_coroutine(p) != MCO_SUCCESS) {
        free(p);
        self->impl = NULL;
        return -1;
    }

    p->state = P_PARSER_PAUSE;
    self->impl = p;
    return 0;
}

enum p_parser_state p_parser_feed(struct p_parser *self, size_t len, const char *str) {
    if (!self || !self->impl) return P_PARSER_FAIL;
    struct p_parser_impl *p = self->impl;

    if (p->state == P_PARSER_FAIL || p->state == P_PARSER_SUCC) {
        return p->state;
    }

    p->in_buf = str;
    p->in_len = len;
    p->in_pos = 0;
    if (len == 0) p->eof = 1;

    mco_result res = mco_resume(p->co);
    if (res != MCO_SUCCESS) {
        p->state = P_PARSER_FAIL;
        snprintf(p->errmsg, sizeof p->errmsg, "coroutine error: %s", mco_result_description(res));
        return p->state;
    }

    if (mco_status(p->co) == MCO_DEAD) {
        p->state = p->parse_ok ? P_PARSER_SUCC : P_PARSER_FAIL;
    } else {
        p->state = P_PARSER_PAUSE;
    }
    return p->state;
}

struct pnode p_parser_get_result(struct p_parser *self) {
    if (!self || !self->impl) return invalid_result();
    struct p_parser_impl *p = self->impl;

    if (p->state != P_PARSER_SUCC) return invalid_result();

    struct pnode result = p->result;
    p->result = pnode_make_integ(0); /* ownership moved out; leave a safe, empty value behind */

    mco_destroy(p->co);
    p->co = NULL;

    if (start_coroutine(p) != MCO_SUCCESS) {
        p->state = P_PARSER_FAIL;
        snprintf(p->errmsg, sizeof p->errmsg, "out of memory reinitializing parser");
        return result; /* still hand back the value already parsed */
    }

    p->state = P_PARSER_PAUSE;
    p->in_buf = NULL;
    p->in_len = 0;
    p->in_pos = 0;
    p->eof = 0;
    p->parse_ok = 0;
    p->errmsg[0] = '\0';
    /* Should already be empty (every path pops/clears before the
     * coroutine returns) - reset defensively anyway. */
    reclaim_in_flight(p);
    return result;
}

const char *p_parser_errmsg(const struct p_parser *self) {
    if (!self || !self->impl) return "";
    return self->impl->errmsg;
}

void p_parser_destroy(struct p_parser *self) {
    if (!self || !self->impl) return;
    struct p_parser_impl *p = self->impl;

    /* Must run before mco_destroy(): on an abandoned mid-parse, the
     * pointers being reclaimed here point into the coroutine's own
     * stack, which mco_destroy() deallocates. */
    reclaim_in_flight(p);
    pnode_drop(&p->result);

    if (p->co) mco_destroy(p->co);
    free(p);
    self->impl = NULL;
}

struct pnode pexpr_parse(const char *buf, size_t len) {
    struct p_parser parser;
    if (p_parser_init(&parser) != 0) return invalid_result();

    enum p_parser_state st = p_parser_feed(&parser, len, buf);
    if (st == P_PARSER_PAUSE) {
        st = p_parser_feed(&parser, 0, NULL);
    }

    struct pnode result = invalid_result();
    if (st == P_PARSER_SUCC) {
        result = p_parser_get_result(&parser);
    }

    p_parser_destroy(&parser);
    return result;
}
