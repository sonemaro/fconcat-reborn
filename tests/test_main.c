/**
 * @file test_main.c
 * @brief Main entry point for fconcat test suite
 * 
 * Runs all unit and integration tests based on command line arguments:
 *   --unit        Run only unit tests
 *   --integration Run only integration tests
 *   (no args)     Run all tests
 */

#include <stdio.h>
#include <string.h>

/* External test suites - declare main functions */
extern int test_memory_main(void);
extern int test_filter_main(void);
extern int test_config_main(void);
extern int test_traversal_main(void);

static int run_unit_tests(void)
{
    int failed = 0;
    
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "     UNIT TESTS\n");
    fprintf(stderr, "========================================\n");
    
    /* Memory tests */
    fprintf(stderr, "\n>>> Running memory tests...\n");
    failed += test_memory_main();
    
    /* Filter tests */
    fprintf(stderr, "\n>>> Running filter tests...\n");
    failed += test_filter_main();
    
    /* Config tests */
    fprintf(stderr, "\n>>> Running config tests...\n");
    failed += test_config_main();
    
    return failed;
}

static int run_integration_tests(void)
{
    int failed = 0;
    
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "     INTEGRATION TESTS\n");
    fprintf(stderr, "========================================\n");
    
    /* Traversal integration tests */
    fprintf(stderr, "\n>>> Running traversal tests...\n");
    failed += test_traversal_main();
    
    return failed;
}

int main(int argc, char **argv)
{
    int run_unit = 1;
    int run_integration = 1;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--unit") == 0) {
            run_unit = 1;
            run_integration = 0;
        } else if (strcmp(argv[i], "--integration") == 0) {
            run_unit = 0;
            run_integration = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--unit|--integration]\n", argv[0]);
            fprintf(stderr, "  --unit         Run only unit tests\n");
            fprintf(stderr, "  --integration  Run only integration tests\n");
            fprintf(stderr, "  (default)      Run all tests\n");
            return 0;
        }
    }
    
    int total_failed = 0;
    
    if (run_unit) {
        total_failed += run_unit_tests();
    }
    
    if (run_integration) {
        total_failed += run_integration_tests();
    }
    
    fprintf(stderr, "\n========================================\n");
    if (total_failed == 0) {
        fprintf(stderr, "     ALL TESTS PASSED\n");
    } else {
        fprintf(stderr, "     %d TEST SUITE(S) FAILED\n", total_failed);
    }
    fprintf(stderr, "========================================\n\n");
    
    return total_failed > 0 ? 1 : 0;
}
