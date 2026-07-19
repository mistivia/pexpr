#include "pexpr.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static struct pnode *parse(const char *s) {
    return pexpr_parse(s, strlen(s));
}

static void test_parse_integers(void) {
    struct pnode *n;

    n = parse("0");
    CHECK(n && n->type == PTYPE_INTEG);
    CHECK_EQ_LL(n->integ, 0);
    pnode_delete(n);

    n = parse("42");
    CHECK(n && n->type == PTYPE_INTEG);
    CHECK_EQ_LL(n->integ, 42);
    pnode_delete(n);

    n = parse("-42");
    CHECK(n && n->type == PTYPE_INTEG);
    CHECK_EQ_LL(n->integ, -42);
    pnode_delete(n);

    n = parse("+7");
    CHECK(n && n->type == PTYPE_INTEG);
    CHECK_EQ_LL(n->integ, 7);
    pnode_delete(n);
}

static void test_parse_reals(void) {
    struct pnode *n;

    n = parse("1.5");
    CHECK(n && n->type == PTYPE_REAL);
    CHECK_EQ_DBL(n->real, 1.5);
    pnode_delete(n);

    n = parse("1e10");
    CHECK(n && n->type == PTYPE_REAL);
    CHECK_EQ_DBL(n->real, 1e10);
    pnode_delete(n);

    n = parse("-2.5E-3");
    CHECK(n && n->type == PTYPE_REAL);
    CHECK_EQ_DBL(n->real, -2.5E-3);
    pnode_delete(n);

    /* No '.' or 'e' -> integer, per DESIGN's lenient number rule. */
    n = parse("100");
    CHECK(n && n->type == PTYPE_INTEG);
    pnode_delete(n);
}

static void test_parse_strings(void) {
    struct pnode *n;

    n = parse("\"hello\"");
    CHECK(n && n->type == PTYPE_STR);
    CHECK_EQ_LL(n->str_len, 5);
    CHECK(memcmp(n->str, "hello", 5) == 0);
    pnode_delete(n);

    n = parse("\"\\0\\\\\\a\\b\\t\\n\\v\\f\\r\\\"\"");
    CHECK(n && n->type == PTYPE_STR);
    CHECK_EQ_LL(n->str_len, 10);
    CHECK(memcmp(n->str, "\0\\\a\b\t\n\v\f\r\"", 10) == 0);
    pnode_delete(n);

    /* Lowercase and uppercase hex digits both accepted when parsing. */
    n = parse("\"\\x41\\x4A\"");
    CHECK(n && n->type == PTYPE_STR);
    CHECK_EQ_LL(n->str_len, 2);
    CHECK(memcmp(n->str, "AJ", 2) == 0);
    pnode_delete(n);

    /* Lenient: raw multi-line, raw UTF-8 bytes need no escaping. */
    const char raw[] = "\"line1\nline2\xc3\xa9\"";
    n = pexpr_parse(raw, sizeof(raw) - 1);
    CHECK(n && n->type == PTYPE_STR);
    CHECK_EQ_LL(n->str_len, 13);
    CHECK(memcmp(n->str, "line1\nline2\xc3\xa9", 13) == 0);
    pnode_delete(n);

    n = parse("\"\"");
    CHECK(n && n->type == PTYPE_STR);
    CHECK_EQ_LL(n->str_len, 0);
    pnode_delete(n);
}

static void test_parse_list_example_from_design(void) {
    struct pnode *n = parse("[1 2 [4 5 \"str\"]]");
    CHECK(n && n->type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(n), 3);
    CHECK_EQ_LL(n->list[0].integ, 1);
    CHECK_EQ_LL(n->list[1].integ, 2);
    CHECK(n->list[2].type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n->list[2]), 3);
    CHECK_EQ_LL(n->list[2].list[0].integ, 4);
    CHECK_EQ_LL(n->list[2].list[1].integ, 5);
    CHECK(memcmp(n->list[2].list[2].str, "str", 3) == 0);
    pnode_delete(n);
}

static void test_parse_list_lenient_separators(void) {
    struct pnode *n = parse("[1\n2\t3\r4   5]");
    CHECK(n && n->type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(n), 5);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ_LL(n->list[i].integ, i + 1);
    }
    pnode_delete(n);

    n = parse("[   ]");
    CHECK(n && n->type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(n), 0);
    pnode_delete(n);

    n = parse("  [1]  ");
    CHECK(n && n->type == PTYPE_LIST);
    pnode_delete(n);
}

static void test_parse_errors(void) {
    CHECK(parse("") == NULL);
    CHECK(parse("[1 2") == NULL);
    CHECK(parse("\"unterminated") == NULL);
    CHECK(parse("\"bad\\qescape\"") == NULL);
    CHECK(parse("\"bad\\x1\"") == NULL);
    CHECK(parse("garbage") == NULL);
    CHECK(parse("]") == NULL);
    CHECK(parse("-") == NULL);
    CHECK(parse(".") == NULL);
    CHECK(parse("1.2.3") == NULL); /* strtod stops early -> trailing junk */
    CHECK(parse("1e") == NULL);    /* exponent marker with no digits */
}

static void test_parse_deep_nesting(void) {
    /* Comfortably within the depth guard. */
    char buf[2048];
    size_t pos = 0;
    for (int i = 0; i < 20; i++) buf[pos++] = '[';
    buf[pos++] = '1';
    for (int i = 0; i < 20; i++) buf[pos++] = ']';
    buf[pos] = '\0';

    struct pnode *n = pexpr_parse(buf, pos);
    CHECK(n != NULL);
    pnode_delete(n);

    /* Well past the depth guard: must fail cleanly, not crash. */
    char *deep = malloc(2000);
    size_t dp = 0;
    for (int i = 0; i < 900; i++) deep[dp++] = '[';
    deep[dp++] = '1';
    for (int i = 0; i < 900; i++) deep[dp++] = ']';

    struct pnode *bad = pexpr_parse(deep, dp);
    CHECK(bad == NULL);
    free(deep);
}

static void test_round_trip(void) {
    const char *inputs[] = {
        "0", "-1", "123456789", "1.5", "-3.25e+10", "\"a\\nb\"",
        "[1 2 3]", "[]", "[[1] [2] [[3]]]", "[\"x\" 1 2.5 [\"y\"]]",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        struct pnode *n1 = parse(inputs[i]);
        CHECK(n1 != NULL);
        if (!n1) continue;

        char *s1 = pexpr_serialize(n1, NULL);
        CHECK(s1 != NULL);

        struct pnode *n2 = pexpr_parse(s1, strlen(s1));
        CHECK(n2 != NULL);

        char *s2 = pexpr_serialize(n2, NULL);
        CHECK(s2 != NULL);

        /* Re-serializing a parsed value must be stable. */
        CHECK_STREQ(s1, s2);

        free(s1);
        free(s2);
        pnode_delete(n1);
        pnode_delete(n2);
    }
}

void run_parser_tests(void) {
    test_parse_integers();
    test_parse_reals();
    test_parse_strings();
    test_parse_list_example_from_design();
    test_parse_list_lenient_separators();
    test_parse_errors();
    test_parse_deep_nesting();
    test_round_trip();
}
