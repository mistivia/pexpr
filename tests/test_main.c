#include "test.h"

int g_test_count = 0;
int g_test_failures = 0;

void run_pnode_tests(void);
void run_serialize_tests(void);
void run_parser_tests(void);
void run_stream_tests(void);

int main(void) {
    run_pnode_tests();
    run_serialize_tests();
    run_parser_tests();
    run_stream_tests();

    printf("\n%d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
