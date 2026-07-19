#include "pbuf.h"

#include <stdlib.h>
#include <string.h>

void pbuf_init(struct pbuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int pbuf_reserve(struct pbuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 0;

    size_t new_cap = b->cap ? b->cap : 16;
    while (new_cap < b->len + extra + 1) new_cap *= 2;

    char *new_data = realloc(b->data, new_cap);
    if (!new_data) return -1;

    b->data = new_data;
    b->cap = new_cap;
    return 0;
}

int pbuf_putc(struct pbuf *b, char c) {
    if (pbuf_reserve(b, 1) != 0) return -1;
    b->data[b->len++] = c;
    return 0;
}

int pbuf_write(struct pbuf *b, const char *s, size_t n) {
    if (pbuf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 0;
}

void pbuf_free(struct pbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

char *pbuf_release(struct pbuf *b, size_t *out_len) {
    if (pbuf_reserve(b, 0) != 0) return NULL; /* ensures room for the NUL */
    b->data[b->len] = '\0';
    if (out_len) *out_len = b->len;
    char *data = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return data;
}
