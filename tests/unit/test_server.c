/**
 * @file test_server.c
 * @brief Unit tests for server internals
 */

#include "test_framework.h"

#ifdef FCONCAT_LEAK_GUARD
extern int server_test_request_options_cleanup_corrupt_counts(void);
#endif

TEST(server_request_options_cleanup_handles_corrupt_counts)
{
#ifndef FCONCAT_LEAK_GUARD
    return 0;
#else
    ASSERT_EQ(0, server_test_request_options_cleanup_corrupt_counts());
    return 0;
#endif
}

int test_server_main(void)
{
    TEST_SUITE_BEGIN("Server Safety");
    RUN_TEST(server_request_options_cleanup_handles_corrupt_counts);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
