#include "pexpr.h"
#include "test.h"

#include <string.h>

static void test_stream_byte_by_byte_list(void) {
    const char *doc = "[1 2 [4 5 \"str\"]]";
    size_t len = strlen(doc);

    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    enum p_parser_state st = P_PARSER_PAUSE;
    for (size_t i = 0; i < len; i++) {
        st = p_parser_feed(&p, 1, doc + i);
        if (i + 1 < len) {
            CHECK(st == P_PARSER_PAUSE);
        }
    }
    CHECK(st == P_PARSER_SUCC);

    struct pnode *n = p_parser_get_result(&p);
    CHECK(n != NULL);
    if (n) {
        CHECK_EQ_LL(pnode_list_len(n), 3);
        CHECK_EQ_LL(n->list[0]->integ, 1);
        CHECK_EQ_LL(n->list[1]->integ, 2);
        CHECK_EQ_LL(pnode_list_len(n->list[2]), 3);
        pnode_free(n);
    }

    p_parser_destroy(&p);
}

static void test_stream_bare_number_needs_eof(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    CHECK(p_parser_feed(&p, 1, "4") == P_PARSER_PAUSE);
    CHECK(p_parser_feed(&p, 1, "2") == P_PARSER_PAUSE);
    CHECK(p_parser_feed(&p, 0, NULL) == P_PARSER_SUCC);

    struct pnode *n = p_parser_get_result(&p);
    CHECK(n && n->type == PTYPE_INTEG);
    if (n) CHECK_EQ_LL(n->integ, 42);
    pnode_free(n);

    p_parser_destroy(&p);
}

static void test_stream_real_needs_eof(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    const char *tok = "3.5e1";
    for (size_t i = 0; i < strlen(tok); i++) {
        CHECK(p_parser_feed(&p, 1, tok + i) == P_PARSER_PAUSE);
    }
    CHECK(p_parser_feed(&p, 0, NULL) == P_PARSER_SUCC);

    struct pnode *n = p_parser_get_result(&p);
    CHECK(n && n->type == PTYPE_REAL);
    if (n) CHECK_EQ_DBL(n->real, 35.0);
    pnode_free(n);

    p_parser_destroy(&p);
}

static void test_stream_reuse(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    CHECK(p_parser_feed(&p, 5, "[1 2]") == P_PARSER_SUCC);
    struct pnode *n1 = p_parser_get_result(&p);
    CHECK(n1 && pnode_list_len(n1) == 2);
    pnode_free(n1);

    CHECK(p_parser_feed(&p, 5, "[3 4]") == P_PARSER_SUCC);
    struct pnode *n2 = p_parser_get_result(&p);
    CHECK(n2 && pnode_list_len(n2) == 2);
    if (n2) CHECK_EQ_LL(n2->list[0]->integ, 3);
    pnode_free(n2);

    /* get_result() on an already-consumed success returns NULL. */
    CHECK(p_parser_get_result(&p) == NULL);

    p_parser_destroy(&p);
}

static void test_stream_fail(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    enum p_parser_state st = p_parser_feed(&p, 7, "garbage");
    CHECK(st == P_PARSER_FAIL);
    CHECK(strlen(p_parser_errmsg(&p)) > 0);
    CHECK(p_parser_get_result(&p) == NULL);

    /* The terminal state persists across further feeds. */
    CHECK(p_parser_feed(&p, 1, "x") == P_PARSER_FAIL);

    p_parser_destroy(&p);
}

static void test_stream_fail_unterminated(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    CHECK(p_parser_feed(&p, 5, "\"abc") == P_PARSER_PAUSE);
    CHECK(p_parser_feed(&p, 0, NULL) == P_PARSER_FAIL);

    p_parser_destroy(&p);
}

static void test_stream_destroy_midparse(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);
    CHECK(p_parser_feed(&p, 3, "[1 ") == P_PARSER_PAUSE);
    p_parser_destroy(&p);
    /* No crash, no leak - verified when this suite runs under ASan. */
}

static void test_stream_destroy_midtoken(void) {
    /* Pause with a partially-collected number still sitting in the
     * parser's scratch buffer (not yet turned into a pnode), then
     * abandon it. Exercises the `active_buf` reclaim path, distinct from
     * abandoning between list elements. */
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);
    CHECK(p_parser_feed(&p, 2, "[1") == P_PARSER_PAUSE);
    p_parser_destroy(&p);
}

static void test_stream_destroy_never_fed(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);
    p_parser_destroy(&p);
}

static void test_stream_chunked_escape(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    /* Split right across the backslash and inside the \x hex digits. */
    const char *chunks[] = {"\"a", "\\", "x", "4", "1", "b\""};
    enum p_parser_state st = P_PARSER_PAUSE;
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        st = p_parser_feed(&p, strlen(chunks[i]), chunks[i]);
    }
    CHECK(st == P_PARSER_SUCC);

    struct pnode *n = p_parser_get_result(&p);
    CHECK(n && n->type == PTYPE_STR);
    if (n) {
        CHECK_EQ_LL(n->str_len, 3);
        CHECK(memcmp(n->str, "aAb", 3) == 0);
    }
    pnode_free(n);

    p_parser_destroy(&p);
}

static void test_stream_chunked_utf8(void) {
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    /* "e\xc3\xa9" ("é") raw inside the string, split one byte at a time. */
    unsigned char full[] = {'"', 'e', 0xc3, 0xa9, '"'};
    enum p_parser_state st = P_PARSER_PAUSE;
    for (size_t i = 0; i < sizeof full; i++) {
        st = p_parser_feed(&p, 1, (const char *)&full[i]);
    }
    CHECK(st == P_PARSER_SUCC);

    struct pnode *n = p_parser_get_result(&p);
    CHECK(n && n->type == PTYPE_STR);
    if (n) {
        CHECK_EQ_LL(n->str_len, 3);
        CHECK(memcmp(n->str, full + 1, 3) == 0);
    }
    pnode_free(n);

    p_parser_destroy(&p);
}

static void test_stream_large_chunks(void) {
    /* Feeding everything in one big chunk should behave the same as
     * feeding it byte by byte. */
    const char *doc = "[1 2 [4 5 \"str\"]]";
    struct p_parser p;
    CHECK_EQ_LL(p_parser_init(&p), 0);

    CHECK(p_parser_feed(&p, strlen(doc), doc) == P_PARSER_SUCC);
    struct pnode *n = p_parser_get_result(&p);
    CHECK(n != NULL);
    if (n) CHECK_EQ_LL(pnode_list_len(n), 3);
    pnode_free(n);

    p_parser_destroy(&p);
}

void run_stream_tests(void) {
    test_stream_byte_by_byte_list();
    test_stream_bare_number_needs_eof();
    test_stream_real_needs_eof();
    test_stream_reuse();
    test_stream_fail();
    test_stream_fail_unterminated();
    test_stream_destroy_midparse();
    test_stream_destroy_midtoken();
    test_stream_destroy_never_fed();
    test_stream_chunked_escape();
    test_stream_chunked_utf8();
    test_stream_large_chunks();
}
