#include "pexpr.h"

#include <stdlib.h>
#include <string.h>

struct pnode *pnode_new_integ(int64_t v) {
    struct pnode *node = malloc(sizeof *node);
    if (!node) return NULL;
    node->type = PTYPE_INTEG;
    node->integ = v;
    return node;
}

struct pnode *pnode_new_real(double v) {
    struct pnode *node = malloc(sizeof *node);
    if (!node) return NULL;
    node->type = PTYPE_REAL;
    node->real = v;
    return node;
}

struct pnode *pnode_new_str(const char *s, size_t len) {
    struct pnode *node = malloc(sizeof *node);
    if (!node) return NULL;

    char *buf = malloc(len + 1);
    if (!buf) {
        free(node);
        return NULL;
    }
    if (len) memcpy(buf, s, len);
    buf[len] = '\0';

    node->type = PTYPE_STR;
    node->str = buf;
    node->str_len = len;
    return node;
}

struct pnode *pnode_new_cstr(const char *s) {
    return pnode_new_str(s, strlen(s));
}

struct pnode *pnode_new_list(void) {
    struct pnode *node = malloc(sizeof *node);
    if (!node) return NULL;
    node->type = PTYPE_LIST;
    node->list = NULL;
    node->list_len = 0;
    return node;
}

int pnode_list_append(struct pnode *list, struct pnode *child) {
    if (!list || list->type != PTYPE_LIST || !child) return -1;

    struct pnode *new_arr = realloc(list->list, (list->list_len + 1) * sizeof *new_arr);
    if (!new_arr) return -1;

    new_arr[list->list_len] = *child; /* move child's contents into the array */
    list->list = new_arr;
    list->list_len++;

    free(child); /* only the now-empty shell; its payload lives in the array */
    return 0;
}

size_t pnode_list_len(const struct pnode *list) {
    if (!list || list->type != PTYPE_LIST) return 0;
    return list->list_len;
}

/* Releases whatever `node` owns without freeing `node` itself - used both
 * for freeing an inline array element (which isn't separately malloc'd)
 * and, via pnode_free(), for the top-level, separately malloc'd node. */
static void pnode_free_contents(struct pnode *node) {
    switch (node->type) {
        case PTYPE_STR:
            free((void *)node->str);
            break;
        case PTYPE_LIST:
            for (size_t i = 0; i < node->list_len; i++) {
                pnode_free_contents(&node->list[i]);
            }
            free(node->list);
            break;
        default:
            break;
    }
}

void pnode_free(struct pnode *node) {
    if (!node) return;
    pnode_free_contents(node);
    free(node);
}

static int pnode_copy_into(struct pnode *dst, const struct pnode *src) {
    dst->type = src->type;

    switch (src->type) {
        case PTYPE_INTEG:
            dst->integ = src->integ;
            return 0;
        case PTYPE_REAL:
            dst->real = src->real;
            return 0;
        case PTYPE_STR: {
            char *buf = malloc(src->str_len + 1);
            if (!buf) return -1;
            if (src->str_len) memcpy(buf, src->str, src->str_len);
            buf[src->str_len] = '\0';
            dst->str = buf;
            dst->str_len = src->str_len;
            return 0;
        }
        case PTYPE_LIST: {
            struct pnode *arr = NULL;
            if (src->list_len) {
                arr = malloc(src->list_len * sizeof *arr);
                if (!arr) return -1;
            }
            size_t i;
            for (i = 0; i < src->list_len; i++) {
                if (pnode_copy_into(&arr[i], &src->list[i]) != 0) {
                    for (size_t j = 0; j < i; j++) pnode_free_contents(&arr[j]);
                    free(arr);
                    return -1;
                }
            }
            dst->list = arr;
            dst->list_len = src->list_len;
            return 0;
        }
        default:
            return -1;
    }
}

struct pnode *pnode_copy(const struct pnode *node) {
    if (!node) return NULL;

    struct pnode *copy = malloc(sizeof *copy);
    if (!copy) return NULL;

    if (pnode_copy_into(copy, node) != 0) {
        free(copy);
        return NULL;
    }
    return copy;
}
