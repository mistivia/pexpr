#include "pexpr.h"

#include <stdlib.h>
#include <string.h>

struct pnode pnode_new_integ(int64_t v) {
    struct pnode node;
    node.type = PTYPE_INTEG;
    node.integ = v;
    return node;
}

struct pnode pnode_new_real(double v) {
    struct pnode node;
    node.type = PTYPE_REAL;
    node.real = v;
    return node;
}

struct pnode pnode_new_str(const char *s, size_t len) {
    struct pnode node;
    node.type = PTYPE_STR;

    char *buf = malloc(len + 1);
    if (!buf) {
        node.str = NULL;
        node.str_len = 0;
        return node;
    }
    if (len) memcpy(buf, s, len);
    buf[len] = '\0';

    node.str = buf;
    node.str_len = len;
    return node;
}

struct pnode pnode_new_cstr(const char *s) {
    return pnode_new_str(s, strlen(s));
}

struct pnode pnode_new_list(void) {
    struct pnode node;
    node.type = PTYPE_LIST;
    node.list = NULL;
    node.list_len = 0;
    return node;
}

int pnode_ok(const struct pnode *node) {
    switch (node->type) {
        case PTYPE_STR:
            return node->str != NULL;
        case PTYPE_LIST:
            return node->list != NULL || node->list_len == 0;
        default:
            return 1;
    }
}

int pnode_list_append(struct pnode *list, struct pnode child) {
    if (!list || list->type != PTYPE_LIST) return -1;

    struct pnode *new_arr = realloc(list->list, (list->list_len + 1) * sizeof *new_arr);
    if (!new_arr) return -1;

    new_arr[list->list_len] = child; /* move child's contents into the array */
    list->list = new_arr;
    list->list_len++;
    return 0;
}

size_t pnode_list_len(const struct pnode *list) {
    if (!list || list->type != PTYPE_LIST) return 0;
    return list->list_len;
}

void pnode_free(struct pnode *node) {
    if (!node) return;

    switch (node->type) {
        case PTYPE_STR:
            free((void *)node->str);
            node->str = NULL;
            node->str_len = 0;
            break;
        case PTYPE_LIST:
            if (node->list) {
                for (size_t i = 0; i < node->list_len; i++) {
                    pnode_free(&node->list[i]);
                }
            }
            free(node->list);
            node->list = NULL;
            node->list_len = 0;
            break;
        default:
            break;
    }
}

void pnode_delete(struct pnode *node) {
    if (!node) return;
    pnode_free(node);
    free(node);
}

struct pnode pnode_copy(const struct pnode *node) {
    struct pnode copy;
    copy.type = node->type;

    switch (node->type) {
        case PTYPE_INTEG:
            copy.integ = node->integ;
            return copy;
        case PTYPE_REAL:
            copy.real = node->real;
            return copy;
        case PTYPE_STR: {
            char *buf = malloc(node->str_len + 1);
            if (!buf) {
                copy.str = NULL;
                copy.str_len = 0;
                return copy;
            }
            if (node->str_len) memcpy(buf, node->str, node->str_len);
            buf[node->str_len] = '\0';
            copy.str = buf;
            copy.str_len = node->str_len;
            return copy;
        }
        case PTYPE_LIST: {
            struct pnode *arr = NULL;
            if (node->list_len) {
                arr = malloc(node->list_len * sizeof *arr);
                if (!arr) {
                    copy.list = NULL;
                    copy.list_len = (size_t)-1; /* failure marker: see pnode_ok() */
                    return copy;
                }
            }
            size_t i;
            for (i = 0; i < node->list_len; i++) {
                arr[i] = pnode_copy(&node->list[i]);
                if (!pnode_ok(&arr[i])) {
                    for (size_t j = 0; j <= i; j++) pnode_free(&arr[j]);
                    free(arr);
                    copy.list = NULL;
                    copy.list_len = (size_t)-1;
                    return copy;
                }
            }
            copy.list = arr;
            copy.list_len = node->list_len;
            return copy;
        }
        default:
            copy.list = NULL;
            copy.list_len = (size_t)-1;
            return copy;
    }
}
