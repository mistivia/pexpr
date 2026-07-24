#include "pexpr.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static struct pnode parse(const char *s) {
    return pexpr_parse(s, strlen(s));
}

static void test_parse_symbols(void) {
    struct pnode n = parse("nil");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 3);
    CHECK(memcmp(n.str, "nil", 3) == 0);
    pnode_drop(&n);

    n = parse("[nil 1 nil]");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n), 3);
    CHECK(n.list[0].type == PTYPE_SYMBOL);
    CHECK(n.list[1].type == PTYPE_INTEG);
    CHECK(n.list[2].type == PTYPE_SYMBOL);
    pnode_drop(&n);

    n = parse("foo-bar_baz*?!");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 14);
    CHECK(memcmp(n.str, "foo-bar_baz*?!", 14) == 0);
    pnode_drop(&n);

    /* Now that nil is just a symbol, any bareword matching the grammar
     * parses fine, including former "not nil" edge cases. */
    n = parse("ni");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 2);
    pnode_drop(&n);

    n = parse("nix");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 3);
    pnode_drop(&n);

    n = parse("garbage");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 7);
    pnode_drop(&n);

    /* A symbol stops at the first byte outside the grammar, here the ']'. */
    n = parse("[ab! cd]");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n), 2);
    CHECK(n.list[0].type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.list[0].str_len, 3);
    CHECK(memcmp(n.list[0].str, "ab!", 3) == 0);
    pnode_drop(&n);

    /* Every <special initial> byte can start a symbol. */
    {
        const char *sym = "<foo?>=/*&%$~^:baz";
        n = parse(sym);
        CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
        CHECK_EQ_LL(n.str_len, strlen(sym));
        CHECK(memcmp(n.str, sym, strlen(sym)) == 0);
        pnode_drop(&n);
    }

    /* <subsequent> additionally allows digits, '.', '+', '-', '@' after the
     * first byte. */
    {
        const char *sym = "list->vector-1.0@x";
        n = parse(sym);
        CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
        CHECK_EQ_LL(n.str_len, strlen(sym));
        CHECK(memcmp(n.str, sym, strlen(sym)) == 0);
        pnode_drop(&n);
    }

    /* Peculiar identifiers: a bare "+" or "-" is a symbol, not a malformed
     * number. */
    n = parse("+");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 1);
    CHECK(n.str[0] == '+');
    pnode_drop(&n);

    n = parse("-");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 1);
    CHECK(n.str[0] == '-');
    pnode_drop(&n);

    n = parse("[+ -]");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n), 2);
    CHECK(n.list[0].type == PTYPE_SYMBOL);
    CHECK(n.list[1].type == PTYPE_SYMBOL);
    pnode_drop(&n);

    /* Symbols are case-insensitive: uppercase letters fold to lowercase. */
    n = parse("NIL");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 3);
    CHECK(memcmp(n.str, "nil", 3) == 0);
    pnode_drop(&n);

    n = parse("Set!");
    CHECK(pnode_ok(&n) && n.type == PTYPE_SYMBOL);
    CHECK_EQ_LL(n.str_len, 4);
    CHECK(memcmp(n.str, "set!", 4) == 0);
    pnode_drop(&n);
}

static void test_parse_integers(void) {
    struct pnode n;

    n = parse("0");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, 0);
    pnode_drop(&n);

    n = parse("42");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, 42);
    pnode_drop(&n);

    n = parse("-42");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, -42);
    pnode_drop(&n);

    n = parse("+7");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, 7);
    pnode_drop(&n);
}

static void test_parse_reals(void) {
    struct pnode n;

    n = parse("1.5");
    CHECK(pnode_ok(&n) && n.type == PTYPE_REAL);
    CHECK_EQ_DBL(n.real, 1.5);
    pnode_drop(&n);

    n = parse("1e10");
    CHECK(pnode_ok(&n) && n.type == PTYPE_REAL);
    CHECK_EQ_DBL(n.real, 1e10);
    pnode_drop(&n);

    n = parse("-2.5E-3");
    CHECK(pnode_ok(&n) && n.type == PTYPE_REAL);
    CHECK_EQ_DBL(n.real, -2.5E-3);
    pnode_drop(&n);

    /* Leading-dot reals (no digit before the '.') are accepted. */
    n = parse(".5");
    CHECK(pnode_ok(&n) && n.type == PTYPE_REAL);
    CHECK_EQ_DBL(n.real, 0.5);
    pnode_drop(&n);

    n = parse("-.25");
    CHECK(pnode_ok(&n) && n.type == PTYPE_REAL);
    CHECK_EQ_DBL(n.real, -0.25);
    pnode_drop(&n);

    /* No '.' or 'e' -> integer, per DESIGN's lenient number rule. */
    n = parse("100");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    pnode_drop(&n);
}

static void test_parse_strings(void) {
    struct pnode n;

    n = parse("\"hello\"");
    CHECK(pnode_ok(&n) && n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 5);
    CHECK(memcmp(n.str, "hello", 5) == 0);
    pnode_drop(&n);

    n = parse("\"\\0\\\\\\a\\b\\t\\n\\v\\f\\r\\\"\"");
    CHECK(pnode_ok(&n) && n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 10);
    CHECK(memcmp(n.str, "\0\\\a\b\t\n\v\f\r\"", 10) == 0);
    pnode_drop(&n);

    /* Lowercase and uppercase hex digits both accepted when parsing. */
    n = parse("\"\\x41\\x4A\"");
    CHECK(pnode_ok(&n) && n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 2);
    CHECK(memcmp(n.str, "AJ", 2) == 0);
    pnode_drop(&n);

    /* Lenient: raw multi-line, raw UTF-8 bytes need no escaping. */
    const char raw[] = "\"line1\nline2\xc3\xa9\"";
    n = pexpr_parse(raw, sizeof(raw) - 1);
    CHECK(pnode_ok(&n) && n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 13);
    CHECK(memcmp(n.str, "line1\nline2\xc3\xa9", 13) == 0);
    pnode_drop(&n);

    n = parse("\"\"");
    CHECK(pnode_ok(&n) && n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 0);
    pnode_drop(&n);
}

static void test_parse_list_example_from_design(void) {
    struct pnode n = parse("[1 2 [4 5 \"str\"]]");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n), 3);
    CHECK_EQ_LL(n.list[0].integ, 1);
    CHECK_EQ_LL(n.list[1].integ, 2);
    CHECK(n.list[2].type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n.list[2]), 3);
    CHECK_EQ_LL(n.list[2].list[0].integ, 4);
    CHECK_EQ_LL(n.list[2].list[1].integ, 5);
    CHECK(memcmp(n.list[2].list[2].str, "str", 3) == 0);
    pnode_drop(&n);
}

static void test_parse_list_lenient_separators(void) {
    struct pnode n = parse("[1\n2\t3\r4   5]");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n), 5);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ_LL(n.list[i].integ, i + 1);
    }
    pnode_drop(&n);

    n = parse("[   ]");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n), 0);
    pnode_drop(&n);

    n = parse("  [1]  ");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    pnode_drop(&n);
}

static void test_parse_comments(void) {
    struct pnode n = parse("1 ; trailing comment\n");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, 1);
    pnode_drop(&n);

    n = parse("; leading comment\n42");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, 42);
    pnode_drop(&n);

    n = parse("42 ; comment with no trailing newline");
    CHECK(pnode_ok(&n) && n.type == PTYPE_INTEG);
    CHECK_EQ_LL(n.integ, 42);
    pnode_drop(&n);

    n = parse("[1 2 ;comment\n 3]");
    CHECK(pnode_ok(&n) && n.type == PTYPE_LIST);
    CHECK_EQ_LL(pnode_list_len(&n), 3);
    CHECK_EQ_LL(n.list[2].integ, 3);
    pnode_drop(&n);

    /* ';' inside a string is not a comment marker. */
    n = parse("\"a;b\"");
    CHECK(pnode_ok(&n) && n.type == PTYPE_STR);
    CHECK_EQ_LL(n.str_len, 3);
    CHECK(memcmp(n.str, "a;b", 3) == 0);
    pnode_drop(&n);
}

static void test_parse_errors(void) {
    struct pnode n;

    n = parse(""); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("[1 2"); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("\"unterminated"); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("\"bad\\qescape\""); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("\"bad\\x1\""); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("@garbage"); CHECK(!pnode_ok(&n)); pnode_drop(&n); /* '@' isn't a symbol-initial byte */
    n = parse("]"); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("."); CHECK(!pnode_ok(&n)); pnode_drop(&n);       /* bare '.' is not a valid number */
    n = parse(".."); CHECK(!pnode_ok(&n)); pnode_drop(&n);      /* "..." isn't a supported peculiar symbol */
    n = parse("..."); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("+abc"); CHECK(!pnode_ok(&n)); pnode_drop(&n);    /* '+' only stands alone, not as an initial */
    n = parse("-abc"); CHECK(!pnode_ok(&n)); pnode_drop(&n);
    n = parse("1.2.3"); CHECK(!pnode_ok(&n)); pnode_drop(&n); /* strtod stops early -> trailing junk */
    n = parse("1e"); CHECK(!pnode_ok(&n)); pnode_drop(&n);    /* exponent marker with no digits */
    n = parse("1-2"); CHECK(!pnode_ok(&n)); pnode_drop(&n);   /* integer token, strtoll stops at '-' */
    n = parse("99999999999999999999"); CHECK(!pnode_ok(&n)); pnode_drop(&n); /* strtoll ERANGE */
    n = parse("\"trailing\\"); CHECK(!pnode_ok(&n)); pnode_drop(&n); /* backslash then EOF */
    n = parse("\"\\xZZ\""); CHECK(!pnode_ok(&n)); pnode_drop(&n);     /* first \x hex digit invalid */
}

static void test_parse_deep_nesting(void) {
    /* Comfortably within the depth guard. */
    char buf[2048];
    size_t pos = 0;
    for (int i = 0; i < 20; i++) buf[pos++] = '[';
    buf[pos++] = '1';
    for (int i = 0; i < 20; i++) buf[pos++] = ']';
    buf[pos] = '\0';

    struct pnode n = pexpr_parse(buf, pos);
    CHECK(pnode_ok(&n));
    pnode_drop(&n);

    /* Well past the depth guard: must fail cleanly, not crash. */
    char *deep = malloc(2000);
    size_t dp = 0;
    for (int i = 0; i < 900; i++) deep[dp++] = '[';
    deep[dp++] = '1';
    for (int i = 0; i < 900; i++) deep[dp++] = ']';

    struct pnode bad = pexpr_parse(deep, dp);
    CHECK(!pnode_ok(&bad));
    pnode_drop(&bad);
    free(deep);
}

static void test_round_trip(void) {
    const char *inputs[] = {
        "0", "-1", "123456789", "1.5", "-3.25e+10", "\"a\\nb\"",
        "[1 2 3]", "[]", "[[1] [2] [[3]]]", "[\"x\" 1 2.5 [\"y\"]]",
        "nil", ".5", "-.25", "[nil 1 nil]", "set!", "foo-bar_baz*?!",
        "+", "-", "list->vector", "[+ -]",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        struct pnode n1 = parse(inputs[i]);
        CHECK(pnode_ok(&n1));

        char *s1 = pexpr_serialize(&n1, NULL);
        CHECK(s1 != NULL);

        struct pnode n2 = pexpr_parse(s1, strlen(s1));
        CHECK(pnode_ok(&n2));

        char *s2 = pexpr_serialize(&n2, NULL);
        CHECK(s2 != NULL);

        /* Re-serializing a parsed value must be stable. */
        CHECK_STREQ(s1, s2);

        free(s1);
        free(s2);
        pnode_drop(&n1);
        pnode_drop(&n2);
    }
}

void run_parser_tests(void) {
    test_parse_symbols();
    test_parse_integers();
    test_parse_reals();
    test_parse_strings();
    test_parse_list_example_from_design();
    test_parse_list_lenient_separators();
    test_parse_comments();
    test_parse_errors();
    test_parse_deep_nesting();
    test_round_trip();
}
