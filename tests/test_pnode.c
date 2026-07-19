#include "pexpr.h"
#include "test.h"

#include <string.h>

static void test_integ(void) {
    struct pnode n = pnode_make_integ(-42);
    CHECK(n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, -42);
    CHECK(pnode_ok(&n));
    pnode_drop(&n);
}

static void test_real(void) {
    struct pnode n = pnode_make_real(3.5);
    CHECK(n.type == PTYPE_REAL);
    CHECK_EQ_DBL(n.real, 3.5);
    CHECK(pnode_ok(&n));
    pnode_drop(&n);
}

static void test_str(void) {
    struct pnode n = pnode_make_str("hello\0world", 11);
    CHECK(pnode_ok(&n));
    CHECK(n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 11);
    CHECK(memcmp(n.str, "hello\0world", 11) == 0);
    /* Always NUL-terminated in addition to str_len tracking the real length. */
    CHECK_EQ_LL(n.str[11], '\0');
    pnode_drop(&n);
}

static void test_str_empty(void) {
    struct pnode n = pnode_make_str("", 0);
    CHECK(pnode_ok(&n));
    CHECK_EQ_LL(n.str_len, 0);
    CHECK_EQ_LL(n.str[0], '\0');
    pnode_drop(&n);
}

static void test_cstr(void) {
    struct pnode n = pnode_make_cstr("hello");
    CHECK(pnode_ok(&n));
    CHECK(n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 5);
    CHECK(memcmp(n.str, "hello", 5) == 0);
    pnode_drop(&n);

    struct pnode e = pnode_make_cstr("");
    CHECK(pnode_ok(&e));
    CHECK_EQ_LL(e.str_len, 0);
    pnode_drop(&e);
}

static void test_list_build(void) {
    struct pnode list = pnode_make_list();
    CHECK(list.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&list), 0);
    CHECK(list.list == NULL);
    CHECK(pnode_ok(&list));

    /* pnode_list_append() moves each child's contents into list.list -
     * list.list[i] is a struct pnode *value*, not a pointer. */
    CHECK_EQ_LL(pnode_list_append(&list, pnode_make_integ(1)), 0);
    CHECK_EQ_LL(pnode_list_append(&list, pnode_make_integ(2)), 0);
    CHECK_EQ_LL(pnode_list_append(&list, pnode_make_str("x", 1)), 0);

    CHECK_EQ_LL(pnode_list_len(&list), 3);
    CHECK_EQ_LL(list.list[0].integ, 1);
    CHECK_EQ_LL(list.list[1].integ, 2);
    CHECK(list.list[2].type == PTYPE_STR);

    pnode_drop(&list);
}

static void test_list_nested(void) {
    struct pnode outer = pnode_make_list();
    struct pnode inner = pnode_make_list();
    CHECK_EQ_LL(pnode_list_append(&inner, pnode_make_integ(9)), 0);
    CHECK_EQ_LL(pnode_list_append(&outer, pnode_make_integ(1)), 0);
    CHECK_EQ_LL(pnode_list_append(&outer, inner), 0);

    CHECK_EQ_LL(pnode_list_len(&outer), 2);
    CHECK_EQ_LL(pnode_list_len(&outer.list[1]), 1);
    CHECK_EQ_LL(outer.list[1].list[0].integ, 9);

    pnode_drop(&outer);
}

static void test_list_append_many(void) {
    /* Exercise the growable array's realloc path across several doublings. */
    struct pnode list = pnode_make_list();
    for (int i = 0; i < 200; i++) {
        CHECK_EQ_LL(pnode_list_append(&list, pnode_make_integ(i)), 0);
    }
    CHECK_EQ_LL(pnode_list_len(&list), 200);
    for (int i = 0; i < 200; i++) {
        CHECK_EQ_LL(list.list[i].integ, i);
    }
    pnode_drop(&list);
}

static void test_copy_scalars(void) {
    struct pnode i = pnode_make_integ(-7);
    struct pnode ic = pnode_copy(&i);
    CHECK(pnode_ok(&ic));
    CHECK(ic.type == PTYPE_INTEG);
    CHECK_EQ_LL(ic.integ, -7);
    pnode_drop(&i);
    pnode_drop(&ic);

    struct pnode r = pnode_make_real(2.5);
    struct pnode rc = pnode_copy(&r);
    CHECK(pnode_ok(&rc));
    CHECK_EQ_DBL(rc.real, 2.5);
    pnode_drop(&r);
    pnode_drop(&rc);
}

static void test_copy_string_is_independent(void) {
    struct pnode s = pnode_make_str("hello", 5);
    struct pnode c = pnode_copy(&s);

    CHECK(pnode_ok(&c));
    CHECK(c.str != s.str); /* separate allocation, not just a pointer copy */
    CHECK_EQ_LL(c.str_len, 5);
    CHECK(memcmp(c.str, "hello", 5) == 0);

    /* Freeing the original must not affect the copy (proves the string
     * bytes were actually duplicated, not shared). */
    pnode_drop(&s);
    CHECK(memcmp(c.str, "hello", 5) == 0);
    pnode_drop(&c);
}

static void test_copy_list_is_deep(void) {
    struct pnode inner = pnode_make_list();
    pnode_list_append(&inner, pnode_make_integ(9));
    pnode_list_append(&inner, pnode_make_str("x", 1));

    struct pnode outer = pnode_make_list();
    pnode_list_append(&outer, pnode_make_integ(1));
    pnode_list_append(&outer, inner);

    struct pnode copy = pnode_copy(&outer);
    CHECK(pnode_ok(&copy));
    CHECK_EQ_LL(pnode_list_len(&copy), 2);

    /* The backing arrays, at every depth, must be distinct allocations -
     * that's what makes this a deep copy rather than a shallow memcpy of
     * pointers. */
    CHECK(copy.list != outer.list);
    CHECK(copy.list[1].list != outer.list[1].list);
    CHECK(copy.list[1].list[1].str != outer.list[1].list[1].str);

    /* Freeing the original must leave the whole copy intact. */
    pnode_drop(&outer);
    CHECK_EQ_LL(pnode_list_len(&copy), 2);
    CHECK_EQ_LL(copy.list[0].integ, 1);
    CHECK_EQ_LL(copy.list[1].list[0].integ, 9);
    CHECK(memcmp(copy.list[1].list[1].str, "x", 1) == 0);

    pnode_drop(&copy);
}

static void test_copy_empty_list(void) {
    struct pnode empty = pnode_make_list();
    struct pnode copy = pnode_copy(&empty);
    CHECK(pnode_ok(&copy) && copy.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&copy), 0);
    pnode_drop(&empty);
    pnode_drop(&copy);
}

static void test_misuse(void) {
    struct pnode scalar = pnode_make_integ(1);

    /* Appending onto a non-list, or through a NULL list pointer, fails
     * without touching the caller's child (it was passed by value, so
     * there's nothing for the callee to leave "untouched" - the caller's
     * own copy is simply unaffected either way). */
    CHECK_EQ_LL(pnode_list_append(&scalar, pnode_make_integ(2)), -1);
    CHECK_EQ_LL(pnode_list_append(NULL, pnode_make_integ(2)), -1);

    CHECK_EQ_LL(pnode_list_len(&scalar), 0);
    CHECK_EQ_LL(pnode_list_len(NULL), 0);
    pnode_drop(&scalar);

    /* pnode_drop must be NULL-safe, and safe to call twice in a row. */
    pnode_drop(NULL);
    struct pnode n = pnode_make_str("x", 1);
    pnode_drop(&n);
    pnode_drop(&n);
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
