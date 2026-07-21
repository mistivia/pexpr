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

static void test_symbols(void) {
    struct pnode n = pnode_make_symbol("nil");
    char *s = ser(&n);
    CHECK_STREQ(s, "nil");
    free(s);
    pnode_drop(&n);

    n = pnode_make_symbol("set!");
    s = ser(&n);
    CHECK_STREQ(s, "set!");
    free(s);
    pnode_drop(&n);

    n = pnode_make_symbol("foo-bar_baz*?");
    s = ser(&n);
    CHECK_STREQ(s, "foo-bar_baz*?");
    free(s);
    pnode_drop(&n);

    /* <special initial> bytes and <subsequent>-only bytes (digit, '.', '+',
     * '-', '@') after the first byte. */
    n = pnode_make_symbol("list->vector-1.0@x");
    s = ser(&n);
    CHECK_STREQ(s, "list->vector-1.0@x");
    free(s);
    pnode_drop(&n);

    /* Peculiar identifiers: bare "+" and "-" serialize as themselves. */
    n = pnode_make_symbol("+");
    s = ser(&n);
    CHECK_STREQ(s, "+");
    free(s);
    pnode_drop(&n);

    n = pnode_make_symbol("-");
    s = ser(&n);
    CHECK_STREQ(s, "-");
    free(s);
    pnode_drop(&n);

    /* Uppercase is folded to lowercase at construction time, so what gets
     * serialized is already lowercase regardless of the input case. */
    n = pnode_make_symbol("NIL");
    s = ser(&n);
    CHECK_STREQ(s, "nil");
    free(s);
    pnode_drop(&n);

    n = pnode_make_symbol("Set!");
    s = ser(&n);
    CHECK_STREQ(s, "set!");
    free(s);
    pnode_drop(&n);

    /* Doesn't match <initial> <subsequent>* and isn't a peculiar identifier
     * -> encode error, same treatment as a NaN/Inf real. */
    n = pnode_make_symbol("1abc"); /* digit can't start a symbol */
    CHECK(pexpr_serialize(&n, NULL) == NULL);
    pnode_drop(&n);

    n = pnode_make_symbol("");
    CHECK(pexpr_serialize(&n, NULL) == NULL);
    pnode_drop(&n);

    n = pnode_make_symbol("bad symbol");
    CHECK(pexpr_serialize(&n, NULL) == NULL);
    pnode_drop(&n);

    n = pnode_make_symbol("..."); /* "..." isn't a supported peculiar symbol */
    CHECK(pexpr_serialize(&n, NULL) == NULL);
    pnode_drop(&n);

    n = pnode_make_symbol("+abc"); /* '+' only stands alone */
    CHECK(pexpr_serialize(&n, NULL) == NULL);
    pnode_drop(&n);
}

static void test_integers(void) {
    struct pnode n;
    char *s;

    n = pnode_make_integ(0);
    s = ser(&n);
    CHECK_STREQ(s, "0");
    free(s);
    pnode_drop(&n);

    n = pnode_make_integ(-42);
    s = ser(&n);
    CHECK_STREQ(s, "-42");
    free(s);
    pnode_drop(&n);

    n = pnode_make_integ(INT64_MAX);
    s = ser(&n);
    CHECK_STREQ(s, "9223372036854775807");
    free(s);
    pnode_drop(&n);

    n = pnode_make_integ(INT64_MIN);
    s = ser(&n);
    CHECK_STREQ(s, "-9223372036854775808");
    free(s);
    pnode_drop(&n);
}

static void round_trips_real(double v) {
    struct pnode n = pnode_make_real(v);
    char *s = ser(&n);
    if (s) {
        /* Serialized reals are required to look like scientific notation. */
        CHECK(strchr(s, 'e') != NULL);
        CHECK_EQ_DBL(strtod(s, NULL), v);
    }
    free(s);
    pnode_drop(&n);
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
    struct pnode nan_node = pnode_make_real(NAN);
    CHECK(pexpr_serialize(&nan_node, NULL) == NULL);
    pnode_drop(&nan_node);

    struct pnode inf_node = pnode_make_real(INFINITY);
    CHECK(pexpr_serialize(&inf_node, NULL) == NULL);
    pnode_drop(&inf_node);
}

static void test_string_escapes(void) {
    struct pnode n;
    char *s;

    /* Every named escape from DESIGN, in order. */
    n = pnode_make_str("\0\\\a\b\t\n\v\f\r\"", 10);
    s = ser(&n);
    CHECK_STREQ(s, "\"\\0\\\\\\a\\b\\t\\n\\v\\f\\r\\\"\"");
    free(s);
    pnode_drop(&n);

    /* Printable ASCII passes through unescaped. */
    n = pnode_make_str("Hello, World! 123", 17);
    s = ser(&n);
    CHECK_STREQ(s, "\"Hello, World! 123\"");
    free(s);
    pnode_drop(&n);

    /* Non-printable / non-ASCII bytes become lowercase \xhh, one per byte. */
    unsigned char raw[] = {0x01, 0x1f, 0x7f, 0x80, 0xff};
    n = pnode_make_str((const char *)raw, sizeof raw);
    s = ser(&n);
    CHECK_STREQ(s, "\"\\x01\\x1f\\x7f\\x80\\xff\"");
    free(s);
    pnode_drop(&n);

    /* UTF-8 text gets escaped byte-by-byte (round-trip is the parser's job). */
    const char utf8[] = "caf\xc3\xa9"; /* "café" */
    n = pnode_make_str(utf8, strlen(utf8));
    s = ser(&n);
    CHECK_STREQ(s, "\"caf\\xc3\\xa9\"");
    free(s);
    pnode_drop(&n);

    n = pnode_make_str("", 0);
    s = ser(&n);
    CHECK_STREQ(s, "\"\"");
    free(s);
    pnode_drop(&n);
}

static void test_list_example_from_design(void) {
    /* DESIGN's own example: [1 2 [4 5 "str"]] */
    struct pnode inner = pnode_make_list();
    pnode_list_append(&inner, pnode_make_integ(4));
    pnode_list_append(&inner, pnode_make_integ(5));
    pnode_list_append(&inner, pnode_make_str("str", 3));

    struct pnode outer = pnode_make_list();
    pnode_list_append(&outer, pnode_make_integ(1));
    pnode_list_append(&outer, pnode_make_integ(2));
    pnode_list_append(&outer, inner);

    char *s = ser(&outer);
    CHECK_STREQ(s, "[1 2 [4 5 \"str\"]]");
    free(s);
    pnode_drop(&outer);
}

static void test_list_propagates_child_error(void) {
    /* A NaN nested arbitrarily deep must fail the whole serialization,
     * not just be skipped. */
    struct pnode list = pnode_make_list();
    pnode_list_append(&list, pnode_make_integ(1));
    pnode_list_append(&list, pnode_make_real(NAN));
    CHECK(pexpr_serialize(&list, NULL) == NULL);
    pnode_drop(&list);
}

static void test_list_empty(void) {
    struct pnode n = pnode_make_list();
    char *s = ser(&n);
    CHECK_STREQ(s, "[]");
    free(s);
    pnode_drop(&n);
}

static void test_serialize_null(void) {
    CHECK(pexpr_serialize(NULL, NULL) == NULL);
}

static void test_serialize_invalid_node(void) {
    /* A failed pexpr_parse()/pnode_copy() result embedded in a list must
     * not crash serialization - it has no valid content to walk. */
    struct pnode bad = pexpr_parse("@garbage", 8);
    CHECK(!pnode_ok(&bad));
    CHECK(pexpr_serialize(&bad, NULL) == NULL);
    pnode_drop(&bad);

    struct pnode list = pnode_make_list();
    struct pnode bad2 = pexpr_parse("", 0);
    CHECK_EQ_LL(pnode_list_append(&list, bad2), 0);
    CHECK(pexpr_serialize(&list, NULL) == NULL);
    pnode_drop(&list);
}

void run_serialize_tests(void) {
    test_symbols();
    test_integers();
    test_reals();
    test_string_escapes();
    test_list_example_from_design();
    test_list_propagates_child_error();
    test_list_empty();
    test_serialize_null();
    test_serialize_invalid_node();
}
