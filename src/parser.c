#include "pexpr.h"
#include "pbuf.h"
#include "stackless.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounds the coroutine's explicit call stack against unbounded recursion. */
#define PEXPR_MAX_DEPTH 256

/* Token-collector modes for the shared P_TOKEN procedure. */
enum tok_mode {
    TOK_NUMBER, /* leading digit or '.': always a number */
    TOK_SYMBOL, /* leading <initial>: always a symbol */
    TOK_NUMSYM  /* leading '+'/'-': a peculiar symbol or a signed number */
};

struct p_parser_impl {
    /* Must be the first member: parser_co() recovers `impl` by casting its
     * `struct co *` argument straight back to this type. */
    struct co co;

    enum p_parser_state state;

    /* Current chunk being fed; only valid while inside parser_co(). */
    const char *in_buf;
    size_t in_len;
    size_t in_pos;
    int eof;

    /* Comment-skipping state, persisted across feed() chunks so a comment
     * that straddles a chunk boundary keeps being skipped on resume.
     * in_string suppresses comment skipping while a string's contents are
     * being collected. */
    int in_comment;
    int in_string;

    /* Result of the most recent P_PEEK: peek_have is 1 for a byte in
     * peek_c, or 0 at EOF. Kept in the impl, not a local, so a suspension in
     * the middle of a peek loses nothing. */
    unsigned char peek_c;
    int peek_have;

    /* Return channel for a CO_CALL'd parse procedure: 1 = success (result
     * written through the frame's `out`), 0 = failure (errmsg set). Read by
     * the caller immediately after the call returns, before anything else
     * can overwrite it. */
    int rc;

    int parse_ok; /* mirrors the top-level parse's rc once the coroutine finishes */
    struct pnode result;
    int result_taken; /* true once p_parser_get_result() has moved `result` out */

    /*
     * Allocations that live in the coroutine's heap-allocated stack frames.
     * Abandoning a parse (reset/destroy mid-stream) makes co_drop() free
     * those frames without running any cleanup, so these must be reclaimed
     * first: `open_lists` is every PTYPE_LIST under construction (pushed/
     * popped around a P_LIST frame); `active_buf` is the one token buffer
     * in progress, if any (token collection never recurses).
     */
    struct pnode **open_lists;
    size_t open_lists_len;
    size_t open_lists_cap;
    struct pbuf *active_buf;

    char errmsg[160];
};

/* "No value" marker, distinguishable via pnode_ok(): a PTYPE_LIST with
 * list == NULL but list_len == (size_t)-1, same convention pnode_copy()
 * uses for its own failure case. */
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

/* Frees whatever's currently in flight; safe any time (already empty
 * after a normal success/failure completion). */
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

static void set_err(struct p_parser_impl *p, const char *msg) {
    snprintf(p->errmsg, sizeof p->errmsg, "%s", msg);
}

/* Clears `active_buf`, frees `buf`, and records an error - the common
 * failure exit while a string or number token is being collected. */
static void set_buf_err(struct p_parser_impl *p, struct pbuf *buf, const char *msg) {
    p->active_buf = NULL;
    p->in_string = 0;
    pbuf_free(buf);
    set_err(p, msg);
}

/* ------------------------------------------------------------------ *
 * Character classes (pure helpers, run outside the coroutine)
 * ------------------------------------------------------------------ */

static int is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_num_char(unsigned char c) {
    return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
           c == '+' || c == '-';
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

static int is_peculiar_symbol(const char *tok, size_t len) {
    return len == 1 && (tok[0] == '+' || tok[0] == '-');
}

/* Decodes a single escape byte `e` (the char after '\\'): returns 1 with
 * the byte in *out for a simple escape, 0 for 'x' (caller reads the two hex
 * digits), or -1 for an unknown escape. */
static int decode_simple_escape(unsigned char e, unsigned char *out) {
    switch (e) {
        case '0':  *out = '\0'; return 1;
        case '\\': *out = '\\'; return 1;
        case 'a':  *out = '\a'; return 1;
        case 'b':  *out = '\b'; return 1;
        case 't':  *out = '\t'; return 1;
        case 'n':  *out = '\n'; return 1;
        case 'v':  *out = '\v'; return 1;
        case 'f':  *out = '\f'; return 1;
        case 'r':  *out = '\r'; return 1;
        case '"':  *out = '"';  return 1;
        case 'x':  return 0;
        default:   return -1;
    }
}

/* Classifies a collected numeric token as a real (if it contains '.'/'e'/
 * 'E') or an integer and parses it. Returns 1 with *out filled, or 0 with
 * *errbuf set. */
static int classify_number(const char *tok, size_t len, struct pnode *out,
                           char *errbuf, size_t errcap) {
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
            snprintf(errbuf, errcap, "invalid number literal");
            return 0;
        }
        *out = pnode_make_real(v);
    } else {
        long long v = strtoll(tok, &end, 10);
        if (end != tok + len || errno == ERANGE) {
            snprintf(errbuf, errcap, "invalid number literal");
            return 0;
        }
        *out = pnode_make_integ((int64_t)v);
    }
    return 1; /* pnode_make_integ()/pnode_make_real() never fail */
}

/* ------------------------------------------------------------------ *
 * Recursive-descent grammar, as coroutine procedures.
 *
 * Each procedure receives its caller-supplied arguments plus its own
 * locals in one frame struct (the *_args types); the caller fills only the
 * leading fields and the procedure initializes the rest. Every procedure
 * signals its outcome by writing p->rc and, on success, *out, then CO_RET.
 * ------------------------------------------------------------------ */

CO_PROC_ID(P_PEEK)
CO_PROC_ID(P_VALUE)
CO_PROC_ID(P_LIST)
CO_PROC_ID(P_STRING)
CO_PROC_ID(P_TOKEN)

struct p_value_args {
    struct pnode *out;
    int depth; /* number of enclosing lists */
};

struct p_list_args {
    struct pnode *out;
    int depth; /* nesting depth of this list (enclosing depth + 1) */
    struct pnode list;  /* built up here */
    struct pnode child; /* scratch slot each child is parsed into */
};

struct p_string_args {
    struct pnode *out;
    struct pbuf buf;
    int hi; /* first nibble of a \xHH escape, held across the chunk boundary */
};

struct p_token_args {
    struct pnode *out;
    int mode; /* enum tok_mode */
    struct pbuf buf;
};

static enum co_state parser_co(struct co *co) {
    struct p_parser_impl *p = (struct p_parser_impl *)co;

    CO_BEGIN(co)

    /* Entry: parse exactly one top-level value into p->result. */
    {
        struct p_value_args va = {0};
        va.out = &p->result;
        va.depth = 0;
        CO_CALL(co, P_VALUE, va);
    }
    p->parse_ok = p->rc;
    CO_DONE(co);

    /* --- P_PEEK: the byte source. Skips comments (';' through the byte
     * before the next '\n', which is left in place) outside string
     * literals, yielding whenever the current chunk is exhausted and EOF
     * has not been signalled. Uses only p-> fields, so a yield loses
     * nothing. --- */
    case P_PEEK: {
        for (;;) {
            while (p->in_pos >= p->in_len && !p->eof) {
                CO_YIELD(co);
            }
            if (p->in_pos >= p->in_len) {
                p->peek_have = 0;
                CO_RET(co);
            }
            unsigned char c = (unsigned char)p->in_buf[p->in_pos];
            if (p->in_comment) {
                if (c == '\n') {
                    p->in_comment = 0;
                    p->peek_c = '\n';
                    p->peek_have = 1;
                    CO_RET(co);
                }
                p->in_pos++;
            } else if (!p->in_string && c == ';') {
                p->in_pos++;
                p->in_comment = 1;
            } else {
                p->peek_c = c;
                p->peek_have = 1;
                CO_RET(co);
            }
        }
    }

    /* --- P_VALUE: skip whitespace, then dispatch on the first byte. --- */
    case P_VALUE: {
        #define VAR(f) CO_VAR(co, struct p_value_args, f)
        CO_CALL0(co, P_PEEK);
        while (p->peek_have && is_ws(p->peek_c)) {
            p->in_pos++;
            CO_CALL0(co, P_PEEK);
        }
        if (!p->peek_have) {
            set_err(p, "unexpected end of input");
            p->rc = 0;
            CO_RET(co);
        }

        if (p->peek_c == '[') {
            struct p_list_args la = {0};
            la.out = VAR(out);
            la.depth = VAR(depth) + 1;
            CO_CALL(co, P_LIST, la);
            CO_RET(co); /* rc / *out set by P_LIST */
        }
        if (p->peek_c == '"') {
            struct p_string_args sa = {0};
            sa.out = VAR(out);
            CO_CALL(co, P_STRING, sa);
            CO_RET(co);
        }
        if (is_symbol_initial(p->peek_c)) {
            struct p_token_args ta = {0};
            ta.out = VAR(out);
            ta.mode = TOK_SYMBOL;
            CO_CALL(co, P_TOKEN, ta);
            CO_RET(co);
        }
        if (p->peek_c == '+' || p->peek_c == '-') {
            struct p_token_args ta = {0};
            ta.out = VAR(out);
            ta.mode = TOK_NUMSYM;
            CO_CALL(co, P_TOKEN, ta);
            CO_RET(co);
        }
        if (p->peek_c == '.' || (p->peek_c >= '0' && p->peek_c <= '9')) {
            struct p_token_args ta = {0};
            ta.out = VAR(out);
            ta.mode = TOK_NUMBER;
            CO_CALL(co, P_TOKEN, ta);
            CO_RET(co);
        }
        set_err(p, "unexpected character");
        p->rc = 0;
        CO_RET(co);
        #undef VAR
    }

    /* --- P_LIST: '[' value* ']'. The child loop is a plain for(;;); it
     * ends by CO_RET (on ']' or an error), never a bare `break`. --- */
    case P_LIST: {
        #define VAR(f) CO_VAR(co, struct p_list_args, f)
        /* P_VALUE dispatches here only with in_pos already on the '['. */
        p->in_pos++; /* consume '[' */

        if (VAR(depth) > PEXPR_MAX_DEPTH) {
            set_err(p, "list nesting too deep");
            p->rc = 0;
            CO_RET(co);
        }

        VAR(list) = pnode_make_list(); /* cannot fail */
        if (push_open_list(p, &VAR(list)) != 0) {
            pnode_drop(&VAR(list));
            set_err(p, "out of memory");
            p->rc = 0;
            CO_RET(co);
        }

        for (;;) {
            CO_CALL0(co, P_PEEK);
            while (p->peek_have && is_ws(p->peek_c)) {
                p->in_pos++;
                CO_CALL0(co, P_PEEK);
            }
            if (!p->peek_have) {
                pop_open_list(p);
                pnode_drop(&VAR(list));
                set_err(p, "unterminated list");
                p->rc = 0;
                CO_RET(co);
            }
            if (p->peek_c == ']') {
                p->in_pos++;
                pop_open_list(p);
                *VAR(out) = VAR(list);
                p->rc = 1;
                CO_RET(co);
            }

            {
                struct p_value_args va = {0};
                va.out = &VAR(child);
                va.depth = VAR(depth);
                CO_CALL(co, P_VALUE, va);
            }
            if (!p->rc) {
                pop_open_list(p);
                pnode_drop(&VAR(list));
                CO_RET(co); /* rc already 0, errmsg already set by the child */
            }
            if (pnode_list_append(&VAR(list), VAR(child)) != 0) {
                pnode_drop(&VAR(child));
                pop_open_list(p);
                pnode_drop(&VAR(list));
                set_err(p, "out of memory");
                p->rc = 0;
                CO_RET(co);
            }
        }
        #undef VAR
    }

    /* --- P_STRING: '"' ... '"' with escapes, collected into a pbuf. --- */
    case P_STRING: {
        #define VAR(f) CO_VAR(co, struct p_string_args, f)
        /* P_VALUE dispatches here only with in_pos already on the '"'. */
        p->in_pos++; /* consume opening '"' */

        pbuf_init(&VAR(buf));
        p->active_buf = &VAR(buf);
        p->in_string = 1;

        for (;;) {
            CO_CALL0(co, P_PEEK);
            if (!p->peek_have) {
                set_buf_err(p, &VAR(buf), "unterminated string");
                p->rc = 0;
                CO_RET(co);
            }
            unsigned char c = p->peek_c;
            p->in_pos++;
            if (c == '"') break; /* binds to this for; finalize below */

            unsigned char val;
            if (c == '\\') {
                CO_CALL0(co, P_PEEK);
                if (!p->peek_have) {
                    set_buf_err(p, &VAR(buf), "unterminated escape sequence");
                    p->rc = 0;
                    CO_RET(co);
                }
                unsigned char e = p->peek_c;
                p->in_pos++;
                int k = decode_simple_escape(e, &val);
                if (k < 0) {
                    set_buf_err(p, &VAR(buf), "unknown escape sequence");
                    p->rc = 0;
                    CO_RET(co);
                }
                if (k == 0) {
                    /* \xHH: the two hex digits may straddle a chunk
                     * boundary, so the first nibble is stashed in the frame. */
                    CO_CALL0(co, P_PEEK);
                    if (p->peek_have) VAR(hi) = hex_val(p->peek_c);
                    if (!p->peek_have || VAR(hi) < 0) {
                        set_buf_err(p, &VAR(buf), "invalid \\x escape");
                        p->rc = 0;
                        CO_RET(co);
                    }
                    p->in_pos++;
                    CO_CALL0(co, P_PEEK);
                    int lo = p->peek_have ? hex_val(p->peek_c) : -1;
                    if (lo < 0) {
                        set_buf_err(p, &VAR(buf), "invalid \\x escape");
                        p->rc = 0;
                        CO_RET(co);
                    }
                    p->in_pos++;
                    val = (unsigned char)((VAR(hi) << 4) | lo);
                }
            } else {
                val = c;
            }
            if (pbuf_putc(&VAR(buf), (char)val) != 0) {
                set_buf_err(p, &VAR(buf), "out of memory");
                p->rc = 0;
                CO_RET(co);
            }
        }

        p->in_string = 0;
        {
            size_t len;
            char *s = pbuf_release(&VAR(buf), &len);
            p->active_buf = NULL;
            if (!s) {
                set_err(p, "out of memory");
                p->rc = 0;
                CO_RET(co);
            }
            struct pnode node = pnode_make_str(s, len);
            free(s);
            if (!pnode_ok(&node)) {
                set_err(p, "out of memory");
                p->rc = 0;
                CO_RET(co);
            }
            *VAR(out) = node;
        }
        p->rc = 1;
        CO_RET(co);
        #undef VAR
    }

    /* --- P_TOKEN: greedily collect a bareword/number token, then classify
     * it per mode. '+'/'-' collect the symbol superset and are sorted out
     * afterward, since a streaming source may only expose one byte at a
     * time. --- */
    case P_TOKEN: {
        #define VAR(f) CO_VAR(co, struct p_token_args, f)
        pbuf_init(&VAR(buf));
        p->active_buf = &VAR(buf);

        for (;;) {
            CO_CALL0(co, P_PEEK);
            if (!p->peek_have) break;
            int matches = (VAR(mode) == TOK_NUMBER)
                              ? is_num_char(p->peek_c)
                              : is_symbol_subsequent(p->peek_c);
            if (!matches) break;
            if (pbuf_putc(&VAR(buf), (char)p->peek_c) != 0) {
                set_buf_err(p, &VAR(buf), "out of memory");
                p->rc = 0;
                CO_RET(co);
            }
            p->in_pos++;
        }

        {
            size_t len;
            char *tok = pbuf_release(&VAR(buf), &len);
            p->active_buf = NULL;
            if (!tok) {
                set_err(p, "out of memory");
                p->rc = 0;
                CO_RET(co);
            }
            /* len > 0: P_VALUE only dispatches here on a byte that also
             * matches the collector's charset. */

            int mode = VAR(mode);
            struct pnode node;

            if (mode == TOK_SYMBOL ||
                (mode == TOK_NUMSYM && is_peculiar_symbol(tok, len))) {
                node = pnode_make_nsymbol(tok, len);
                free(tok);
                if (!pnode_ok(&node)) {
                    set_err(p, "out of memory");
                    p->rc = 0;
                    CO_RET(co);
                }
                *VAR(out) = node;
                p->rc = 1;
                CO_RET(co);
            }

            if (mode == TOK_NUMSYM) {
                /* Reject the symbol-only bytes the number charset excludes. */
                int bad = 0;
                for (size_t i = 0; i < len; i++) {
                    if (!is_num_char((unsigned char)tok[i])) bad = 1;
                }
                if (bad) {
                    free(tok);
                    set_err(p, "invalid number literal");
                    p->rc = 0;
                    CO_RET(co);
                }
            }

            if (!classify_number(tok, len, &node, p->errmsg, sizeof p->errmsg)) {
                free(tok);
                p->rc = 0;
                CO_RET(co);
            }
            free(tok);
            *VAR(out) = node;
            p->rc = 1;
            CO_RET(co);
        }
        #undef VAR
    }

    CO_END(co)
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

/* Fresh P_PARSER_PAUSE state, for right after (re)starting a coroutine.
 * Shared by p_parser_init() and p_parser_reset(). */
static void reset_fields(struct p_parser_impl *p) {
    p->state = P_PARSER_PAUSE;
    p->in_buf = NULL;
    p->in_len = 0;
    p->in_pos = 0;
    p->eof = 0;
    p->in_comment = 0;
    p->in_string = 0;
    p->peek_c = 0;
    p->peek_have = 0;
    p->rc = 0;
    p->parse_ok = 0;
    p->result_taken = 0;
    p->errmsg[0] = '\0';
}

int p_parser_init(struct p_parser *self) {
    if (!self) return -1;

    struct p_parser_impl *p = calloc(1, sizeof *p);
    if (!p) {
        self->impl = NULL;
        return -1;
    }

    co_init(&p->co);
    if (!p->co.stack) {
        free(p);
        self->impl = NULL;
        return -1;
    }

    reset_fields(p);
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

    enum co_state cs = parser_co(&p->co);
    if (cs == CO_FINISHED) {
        p->state = p->parse_ok ? P_PARSER_SUCC : P_PARSER_FAIL;
    } else {
        p->state = P_PARSER_PAUSE;
    }
    return p->state;
}

struct pnode p_parser_get_result(struct p_parser *self) {
    if (!self || !self->impl) return invalid_result();
    struct p_parser_impl *p = self->impl;

    if (p->state != P_PARSER_SUCC || p->result_taken) return invalid_result();

    struct pnode result = p->result;
    p->result = pnode_make_integ(0); /* ownership moved out; leave a safe, empty value behind */
    p->result_taken = 1;
    return result;
}

const char *p_parser_errmsg(const struct p_parser *self) {
    if (!self || !self->impl) return "";
    return self->impl->errmsg;
}

int p_parser_reset(struct p_parser *self) {
    if (!self || !self->impl) return -1;
    struct p_parser_impl *p = self->impl;

    /* Must run before co_drop() frees the frames these point into. */
    reclaim_in_flight(p);
    pnode_drop(&p->result); /* no-op if p_parser_get_result() already took it */

    co_drop(&p->co);
    co_init(&p->co);
    if (!p->co.stack) {
        p->state = P_PARSER_FAIL;
        snprintf(p->errmsg, sizeof p->errmsg, "out of memory reinitializing parser");
        return -1;
    }

    reset_fields(p);
    return 0;
}

void p_parser_destroy(struct p_parser *self) {
    if (!self || !self->impl) return;
    struct p_parser_impl *p = self->impl;

    /* Must run before co_drop() frees the frames these point into. */
    reclaim_in_flight(p);
    pnode_drop(&p->result);

    co_drop(&p->co);
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
