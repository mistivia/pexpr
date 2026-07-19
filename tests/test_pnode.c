#include "pexpr.h"
#include "test.h"

#include <string.h>

static void test_integ(void) {
    struct pnode *n = pnode_new_integ(-42);
    CHECK(n != NULL);
    CHECK(n->type == PTYPE_INTEG);
    CHECK_EQ_LL(n->integ, -42);
    pnode_free(n);
}

static void test_real(void) {
    struct pnode *n = pnode_new_real(3.5);
    CHECK(n != NULL);
    CHECK(n->type == PTYPE_REAL);
    CHECK_EQ_DBL(n->real, 3.5);
    pnode_free(n);
}

static void test_str(void) {
    struct pnode *n = pnode_new_str("hello\0world", 11);
    CHECK(n != NULL);
    CHECK(n->type == PTYPE_STR);
    CHECK_EQ_LL(n->str_len, 11);
    CHECK(memcmp(n->str, "hello\0world", 11) == 0);
    /* Always NUL-terminated in addition to str_len tracking the real length. */
    CHECK_EQ_LL(n->str[11], '\0');
    pnode_free(n);
}

static void test_str_empty(void) {
    struct pnode *n = pnode_new_str("", 0);
    CHECK(n != NULL);
    CHECK_EQ_LL(n->str_len, 0);
    CHECK_EQ_LL(n->str[0], '\0');
    pnode_free(n);
}

static void test_cstr(void) {
    struct pnode *n = pnode_new_cstr("hello");
    CHECK(n != NULL);
    CHECK(n->type == PTYPE_STR);
    CHECK_EQ_LL(n->str_len, 5);
    CHECK(memcmp(n->str, "hello", 5) == 0);
    pnode_free(n);

    struct pnode *e = pnode_new_cstr("");
    CHECK(e != NULL);
    CHECK_EQ_LL(e->str_len, 0);
    pnode_free(e);
}

static void test_list_build(void) {
    struct pnode *list = pnode_new_list();
    CHECK(list != NULL);
    CHECK(list->type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(list), 0);
    CHECK(list->list[0] == NULL);

    CHECK_EQ_LL(pnode_list_append(list, pnode_new_integ(1)), 0);
    CHECK_EQ_LL(pnode_list_append(list, pnode_new_integ(2)), 0);
    CHECK_EQ_LL(pnode_list_append(list, pnode_new_str("x", 1)), 0);

    CHECK_EQ_LL(pnode_list_len(list), 3);
    CHECK_EQ_LL(list->list[0]->integ, 1);
    CHECK_EQ_LL(list->list[1]->integ, 2);
    CHECK(list->list[2]->type == PTYPE_STR);
    CHECK(list->list[3] == NULL);

    pnode_free(list);
}

static void test_list_nested(void) {
    struct pnode *outer = pnode_new_list();
    struct pnode *inner = pnode_new_list();
    CHECK_EQ_LL(pnode_list_append(inner, pnode_new_integ(9)), 0);
    CHECK_EQ_LL(pnode_list_append(outer, pnode_new_integ(1)), 0);
    CHECK_EQ_LL(pnode_list_append(outer, inner), 0);

    CHECK_EQ_LL(pnode_list_len(outer), 2);
    CHECK_EQ_LL(pnode_list_len(outer->list[1]), 1);
    CHECK_EQ_LL(outer->list[1]->list[0]->integ, 9);

    pnode_free(outer);
}

static void test_list_append_many(void) {
    /* Exercise the growable array's realloc path across several doublings. */
    struct pnode *list = pnode_new_list();
    for (int i = 0; i < 200; i++) {
        CHECK_EQ_LL(pnode_list_append(list, pnode_new_integ(i)), 0);
    }
    CHECK_EQ_LL(pnode_list_len(list), 200);
    for (int i = 0; i < 200; i++) {
        CHECK_EQ_LL(list->list[i]->integ, i);
    }
    pnode_free(list);
}

static void test_copy_scalars(void) {
    struct pnode *i = pnode_new_integ(-7);
    struct pnode *ic = pnode_copy(i);
    CHECK(ic != NULL && ic != i);
    CHECK(ic->type == PTYPE_INTEG);
    CHECK_EQ_LL(ic->integ, -7);
    pnode_free(i);
    pnode_free(ic);

    struct pnode *r = pnode_new_real(2.5);
    struct pnode *rc = pnode_copy(r);
    CHECK(rc != NULL && rc != r);
    CHECK_EQ_DBL(rc->real, 2.5);
    pnode_free(r);
    pnode_free(rc);

    CHECK(pnode_copy(NULL) == NULL);
}

static void test_copy_string_is_independent(void) {
    struct pnode *s = pnode_new_str("hello", 5);
    struct pnode *c = pnode_copy(s);

    CHECK(c != NULL && c != s);
    CHECK(c->str != s->str); /* separate allocation, not just a pointer copy */
    CHECK_EQ_LL(c->str_len, 5);
    CHECK(memcmp(c->str, "hello", 5) == 0);

    /* Freeing the original must not affect the copy (proves the string
     * bytes were actually duplicated, not shared). */
    pnode_free(s);
    CHECK(memcmp(c->str, "hello", 5) == 0);
    pnode_free(c);
}

static void test_copy_list_is_deep(void) {
    struct pnode *inner = pnode_new_list();
    pnode_list_append(inner, pnode_new_integ(9));
    pnode_list_append(inner, pnode_new_str("x", 1));

    struct pnode *outer = pnode_new_list();
    pnode_list_append(outer, pnode_new_integ(1));
    pnode_list_append(outer, inner);

    struct pnode *copy = pnode_copy(outer);
    CHECK(copy != NULL && copy != outer);
    CHECK(copy->list != outer->list);
    CHECK_EQ_LL(pnode_list_len(copy), 2);

    /* Every descendant, at every depth, must be a distinct allocation. */
    CHECK(copy->list[0] != outer->list[0]);
    CHECK(copy->list[1] != outer->list[1]);
    CHECK(copy->list[1]->list[0] != outer->list[1]->list[0]);
    CHECK(copy->list[1]->list[1] != outer->list[1]->list[1]);
    CHECK(copy->list[1]->list[1]->str != outer->list[1]->list[1]->str);

    /* Freeing the original must leave the whole copy intact. */
    pnode_free(outer);
    CHECK_EQ_LL(pnode_list_len(copy), 2);
    CHECK_EQ_LL(copy->list[0]->integ, 1);
    CHECK_EQ_LL(copy->list[1]->list[0]->integ, 9);
    CHECK(memcmp(copy->list[1]->list[1]->str, "x", 1) == 0);

    pnode_free(copy);
}

static void test_copy_empty_list(void) {
    struct pnode *empty = pnode_new_list();
    struct pnode *copy = pnode_copy(empty);
    CHECK(copy != NULL && copy->type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(copy), 0);
    pnode_free(empty);
    pnode_free(copy);
}

static void test_misuse(void) {
    struct pnode *scalar = pnode_new_integ(1);

    /* On failure pnode_list_append() leaves the child untouched, so the
     * caller still owns it and must free it separately. */
    struct pnode *orphan1 = pnode_new_integ(2);
    CHECK_EQ_LL(pnode_list_append(scalar, orphan1), -1);
    pnode_free(orphan1);

    struct pnode *orphan2 = pnode_new_integ(2);
    CHECK_EQ_LL(pnode_list_append(NULL, orphan2), -1);
    pnode_free(orphan2);

    CHECK_EQ_LL(pnode_list_len(scalar), 0);
    CHECK_EQ_LL(pnode_list_len(NULL), 0);
    pnode_free(scalar);

    /* pnode_free must be NULL-safe. */
    pnode_free(NULL);
}

void run_pnode_tests(void) {
    test_integ();
    test_real();
    test_str();
    test_str_empty();
    test_cstr();
    test_list_build();
    test_list_nested();
    test_list_append_many();
    test_copy_scalars();
    test_copy_string_is_independent();
    test_copy_list_is_deep();
    test_copy_empty_list();
    test_misuse();
}
