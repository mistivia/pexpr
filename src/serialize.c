#include "pexpr.h"
#include "pbuf.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int emit_integ(struct pbuf *out, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%" PRId64, v);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    return pbuf_write(out, tmp, (size_t)n);
}

/* Formats `v` in scientific notation using the smallest precision that
 * still round-trips exactly through strtod(), so output stays compact
 * without losing information. */
static int emit_real(struct pbuf *out, double v) {
    if (isnan(v) || isinf(v)) return -1;

    char tmp[64];
    for (int prec = 0; prec <= 17; prec++) {
        int n = snprintf(tmp, sizeof tmp, "%.*e", prec, v);
        if (n < 0 || (size_t)n >= sizeof tmp) return -1;
        if (strtod(tmp, NULL) == v) {
            return pbuf_write(out, tmp, (size_t)n);
        }
    }
    return -1;
}

static int emit_str(struct pbuf *out, const char *s, size_t len) {
    if (pbuf_putc(out, '"') != 0) return -1;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc = 0;
        switch (c) {
            case '\0': esc = '0'; break;
            case '\\': esc = '\\'; break;
            case '\a': esc = 'a'; break;
            case '\b': esc = 'b'; break;
            case '\t': esc = 't'; break;
            case '\n': esc = 'n'; break;
            case '\v': esc = 'v'; break;
            case '\f': esc = 'f'; break;
            case '\r': esc = 'r'; break;
            case '"': esc = '"'; break;
            default: break;
        }
        if (esc) {
            if (pbuf_putc(out, '\\') != 0) return -1;
            if (pbuf_putc(out, esc) != 0) return -1;
        } else if (c >= 32 && c <= 126) {
            if (pbuf_putc(out, (char)c) != 0) return -1;
        } else {
            char tmp[5];
            int n = snprintf(tmp, sizeof tmp, "\\x%02x", c);
            if (n != 4) return -1;
            if (pbuf_write(out, tmp, 4) != 0) return -1;
        }
    }

    return pbuf_putc(out, '"');
}

static int emit_node(struct pbuf *out, const struct pnode *node) {
    if (!node) return -1;

    switch (node->type) {
        case PTYPE_INTEG:
            return emit_integ(out, node->integ);
        case PTYPE_REAL:
            return emit_real(out, node->real);
        case PTYPE_STR:
            return emit_str(out, node->str, node->str_len);
        case PTYPE_LIST: {
            if (pbuf_putc(out, '[') != 0) return -1;
            for (size_t i = 0; node->list[i]; i++) {
                if (i > 0 && pbuf_putc(out, ' ') != 0) return -1;
                if (emit_node(out, node->list[i]) != 0) return -1;
            }
            return pbuf_putc(out, ']');
        }
        default:
            return -1;
    }
}

char *pexpr_serialize(const struct pnode *node, size_t *out_len) {
    if (!node) return NULL;

    struct pbuf out;
    pbuf_init(&out);

    if (emit_node(&out, node) != 0) {
        pbuf_free(&out);
        return NULL;
    }

    char *result = pbuf_release(&out, out_len);
    if (!result) {
        pbuf_free(&out);
    }
    return result;
}
