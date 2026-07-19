/* Internal growable byte buffer. Not part of the public API. */
#ifndef PEXPR_PBUF_H
#define PEXPR_PBUF_H

#include <stddef.h>

struct pbuf {
    char *data;
    size_t len;
    size_t cap;
};

void pbuf_init(struct pbuf *b);
int pbuf_putc(struct pbuf *b, char c);
int pbuf_write(struct pbuf *b, const char *s, size_t n);
void pbuf_free(struct pbuf *b);

/* NUL-terminates, hands ownership of the buffer to the caller, and resets
 * `b` to an empty buffer. `out_len` (optional) receives the length
 * excluding the NUL. Returns NULL on allocation failure. */
char *pbuf_release(struct pbuf *b, size_t *out_len);

#endif
