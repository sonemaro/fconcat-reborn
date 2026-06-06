/**
 * @file test_error.c
 * @brief Unit tests for error manager input safety.
 */

#include "test_framework.h"
#include "../../src/core/error.h"
#include <limits.h>

TEST(error_reporting_rejects_null_inputs)
{
    error_report(NULL, FCONCAT_ERROR_INVALID_ARGS, "ignored");
    error_report_context(NULL, FCONCAT_ERROR_INVALID_ARGS, __FILE__, __LINE__, __func__, "ignored");
    warning_report(NULL, "ignored");

    ErrorManager *manager = error_manager_create();
    ASSERT_NOT_NULL(manager);

    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, NULL);
    error_report_context(manager, FCONCAT_ERROR_INVALID_ARGS, __FILE__, __LINE__, __func__, NULL);
    warning_report(manager, NULL);

    ASSERT_EQ(0, error_get_count(manager));
    ASSERT_EQ(0, warning_get_count(manager));

    error_manager_destroy(manager);
    return 0;
}

TEST(error_reporting_respects_capacity)
{
    ErrorManager *manager = error_manager_create();
    ASSERT_NOT_NULL(manager);

    manager->error_count = MAX_ERRORS;
    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "ignored");
    error_report_context(manager, FCONCAT_ERROR_INVALID_ARGS, __FILE__, __LINE__, __func__, "ignored");

    ASSERT_EQ(MAX_ERRORS, error_get_count(manager));

    manager->error_count = 0;
    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "recorded");
    ASSERT_EQ(1, error_get_count(manager));

    error_clear(manager);
    ASSERT_EQ(0, error_get_count(manager));

    error_manager_destroy(manager);
    return 0;
}

TEST(error_reporting_handles_corrupt_counts)
{
    ErrorManager *manager = error_manager_create();
    ASSERT_NOT_NULL(manager);

    manager->error_count = -7;
    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "repaired");
    ASSERT_EQ(1, error_get_count(manager));

    error_clear(manager);
    manager->error_count = MAX_ERRORS + 1;
    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "ignored");
    ASSERT_EQ(MAX_ERRORS, error_get_count(manager));

    error_clear(manager);
    manager->warning_count = -5;
    warning_report(manager, "repaired warning");
    ASSERT_EQ(1, warning_get_count(manager));

    manager->warning_count = INT_MAX;
    warning_report(manager, "saturated warning");
    ASSERT_EQ(INT_MAX, warning_get_count(manager));

    error_manager_destroy(manager);
    return 0;
}

TEST(error_destroy_cleans_entries_with_corrupt_negative_count)
{
    ErrorManager *manager = error_manager_create();
    ASSERT_NOT_NULL(manager);

    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "recorded");
    ASSERT_EQ(1, error_get_count(manager));

    manager->error_count = -1;
    error_manager_destroy(manager);
    return 0;
}

TEST(error_clear_cleans_entries_with_corrupt_negative_count)
{
    ErrorManager *manager = error_manager_create();
    ASSERT_NOT_NULL(manager);

    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "recorded");
    ASSERT_EQ(1, error_get_count(manager));

    manager->error_count = -1;
    error_clear(manager);
    ASSERT_EQ(0, error_get_count(manager));

    error_manager_destroy(manager);
    return 0;
}

TEST(error_append_cleans_entries_before_repairing_negative_count)
{
    ErrorManager *manager = error_manager_create();
    ASSERT_NOT_NULL(manager);

    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "first");
    ASSERT_EQ(1, error_get_count(manager));

    manager->error_count = -1;
    error_report(manager, FCONCAT_ERROR_INVALID_ARGS, "second");
    ASSERT_EQ(1, error_get_count(manager));

    error_manager_destroy(manager);
    return 0;
}

int test_error_main(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    TEST_SUITE_BEGIN("ErrorManager Safety");
    RUN_TEST(error_reporting_rejects_null_inputs);
    RUN_TEST(error_reporting_respects_capacity);
    RUN_TEST(error_reporting_handles_corrupt_counts);
    RUN_TEST(error_destroy_cleans_entries_with_corrupt_negative_count);
    RUN_TEST(error_clear_cleans_entries_with_corrupt_negative_count);
    RUN_TEST(error_append_cleans_entries_before_repairing_negative_count);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
