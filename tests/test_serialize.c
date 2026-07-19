#include "pexpr.h"
#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *ser(const struct pnode *n) {
    size_t len = 0;
    char *s = pexpr_serialize(n, &len);
    CHECK(s != NULL);
    CHECK_EQ_LL(len, s ? strlen(s) : 0);
    return s;
}

static void test_integers(void) {
    struct pnode n;
    char *s;

    n = pnode_new_integ(0);
    s = ser(&n);
    CHECK_STREQ(s, "0");
    free(s);
    pnode_free(&n);

    n = pnode_new_integ(-42);
    s = ser(&n);
    CHECK_STREQ(s, "-42");
    free(s);
    pnode_free(&n);

    n = pnode_new_integ(INT64_MAX);
    s = ser(&n);
    CHECK_STREQ(s, "9223372036854775807");
    free(s);
    pnode_free(&n);

    n = pnode_new_integ(INT64_MIN);
    s = ser(&n);
    CHECK_STREQ(s, "-9223372036854775808");
    free(s);
    pnode_free(&n);
}

static void round_trips_real(double v) {
    struct pnode n = pnode_new_real(v);
    char *s = ser(&n);
    if (s) {
        /* Serialized reals are required to look like scientific notation. */
        CHECK(strchr(s, 'e') != NULL);
        CHECK_EQ_DBL(strtod(s, NULL), v);
    }
    free(s);
    pnode_free(&n);
}

static void test_reals(void) {
    round_trips_real(0.0);
    round_trips_real(-0.0);
    round_trips_real(1.5);
    round_trips_real(2.0);
    round_trips_real(-2.5e10);
    round_trips_real(3.14159265358979);
    round_trips_real(1e100);
    round_trips_real(1e-100);
    round_trips_real(1.0 / 3.0);

    /* NaN/Inf have no decimal representation in this format. */
    struct pnode nan_node = pnode_new_real(NAN);
    CHECK(pexpr_serialize(&nan_node, NULL) == NULL);
    pnode_free(&nan_node);

    struct pnode inf_node = pnode_new_real(INFINITY);
    CHECK(pexpr_serialize(&inf_node, NULL) == NULL);
    pnode_free(&inf_node);
}

static void test_string_escapes(void) {
    struct pnode n;
    char *s;

    /* Every named escape from DESIGN, in order. */
    n = pnode_new_str("\0\\\a\b\t\n\v\f\r\"", 10);
    s = ser(&n);
    CHECK_STREQ(s, "\"\\0\\\\\\a\\b\\t\\n\\v\\f\\r\\\"\"");
    free(s);
    pnode_free(&n);

    /* Printable ASCII passes through unescaped. */
    n = pnode_new_str("Hello, World! 123", 17);
    s = ser(&n);
    CHECK_STREQ(s, "\"Hello, World! 123\"");
    free(s);
    pnode_free(&n);

    /* Non-printable / non-ASCII bytes become lowercase \xhh, one per byte. */
    unsigned char raw[] = {0x01, 0x1f, 0x7f, 0x80, 0xff};
    n = pnode_new_str((const char *)raw, sizeof raw);
    s = ser(&n);
    CHECK_STREQ(s, "\"\\x01\\x1f\\x7f\\x80\\xff\"");
    free(s);
    pnode_free(&n);

    /* UTF-8 text gets escaped byte-by-byte (round-trip is the parser's job). */
    const char utf8[] = "caf\xc3\xa9"; /* "café" */
    n = pnode_new_str(utf8, strlen(utf8));
    s = ser(&n);
    CHECK_STREQ(s, "\"caf\\xc3\\xa9\"");
    free(s);
    pnode_free(&n);

    n = pnode_new_str("", 0);
    s = ser(&n);
    CHECK_STREQ(s, "\"\"");
    free(s);
    pnode_free(&n);
}

static void test_list_example_from_design(void) {
    /* DESIGN's own example: [1 2 [4 5 "str"]] */
    struct pnode inner = pnode_new_list();
    pnode_list_append(&inner, pnode_new_integ(4));
    pnode_list_append(&inner, pnode_new_integ(5));
    pnode_list_append(&inner, pnode_new_str("str", 3));

    struct pnode outer = pnode_new_list();
    pnode_list_append(&outer, pnode_new_integ(1));
    pnode_list_append(&outer, pnode_new_integ(2));
    pnode_list_append(&outer, inner);

    char *s = ser(&outer);
    CHECK_STREQ(s, "[1 2 [4 5 \"str\"]]");
    free(s);
    pnode_free(&outer);
}

static void test_list_propagates_child_error(void) {
    /* A NaN nested arbitrarily deep must fail the whole serialization,
     * not just be skipped. */
    struct pnode list = pnode_new_list();
    pnode_list_append(&list, pnode_new_integ(1));
    pnode_list_append(&list, pnode_new_real(NAN));
    CHECK(pexpr_serialize(&list, NULL) == NULL);
    pnode_free(&list);
}

static void test_list_empty(void) {
    struct pnode n = pnode_new_list();
    char *s = ser(&n);
    CHECK_STREQ(s, "[]");
    free(s);
    pnode_free(&n);
}

static void test_serialize_null(void) {
    CHECK(pexpr_serialize(NULL, NULL) == NULL);
}

void run_serialize_tests(void) {
    test_integers();
    test_reals();
    test_string_escapes();
    test_list_example_from_design();
    test_list_propagates_child_error();
    test_list_empty();
    test_serialize_null();
}
