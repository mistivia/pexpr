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

    struct pnode **list = malloc(sizeof *list);
    if (!list) {
        free(node);
        return NULL;
    }
    list[0] = NULL;

    node->type = PTYPE_LIST;
    node->list = list;
    return node;
}

int pnode_list_append(struct pnode *list, struct pnode *child) {
    if (!list || list->type != PTYPE_LIST || !child) return -1;

    size_t n = 0;
    while (list->list[n]) n++;

    struct pnode **new_list = realloc(list->list, (n + 2) * sizeof *new_list);
    if (!new_list) return -1;

    new_list[n] = child;
    new_list[n + 1] = NULL;

    list->list = new_list;
    return 0;
}

size_t pnode_list_len(const struct pnode *list) {
    if (!list || list->type != PTYPE_LIST) return 0;
    size_t n = 0;
    while (list->list[n]) n++;
    return n;
}

void pnode_free(struct pnode *node) {
    if (!node) return;

    switch (node->type) {
        case PTYPE_STR:
            free((void *)node->str);
            break;
        case PTYPE_LIST:
            for (size_t i = 0; node->list[i]; i++) {
                pnode_free(node->list[i]);
            }
            free(node->list);
            break;
        default:
            break;
    }

    free(node);
}

struct pnode *pnode_copy(const struct pnode *node) {
    if (!node) return NULL;

    switch (node->type) {
        case PTYPE_INTEG:
            return pnode_new_integ(node->integ);
        case PTYPE_REAL:
            return pnode_new_real(node->real);
        case PTYPE_STR:
            return pnode_new_str(node->str, node->str_len);
        case PTYPE_LIST: {
            struct pnode *copy = pnode_new_list();
            if (!copy) return NULL;

            for (size_t i = 0; node->list[i]; i++) {
                struct pnode *child = pnode_copy(node->list[i]);
                if (!child || pnode_list_append(copy, child) != 0) {
                    pnode_free(child);
                    pnode_free(copy);
                    return NULL;
                }
            }
            return copy;
        }
        default:
            return NULL;
    }
}
