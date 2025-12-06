/**
 * @file test_framework.h
 * @brief Minimalist unit test framework for fconcat
 * 
 * A lightweight, zero-dependency test framework providing:
 * - Assertion macros with descriptive failure messages
 * - Test registration and execution
 * - Summary statistics
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static const char *current_test_name = NULL;

/* Color codes for terminal output */
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RESET   "\033[0m"

/* Assertion macros */
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_TRUE(%s) failed" COLOR_RESET "\n", \
                __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while(0)

#define ASSERT_FALSE(cond) do { \
    if ((cond)) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_FALSE(%s) failed" COLOR_RESET "\n", \
                __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while(0)

#define ASSERT_EQ(expected, actual) do { \
    long long _exp = (long long)(expected); \
    long long _act = (long long)(actual); \
    if (_exp != _act) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_EQ(%s, %s) - expected %lld, got %lld" COLOR_RESET "\n", \
                __FILE__, __LINE__, #expected, #actual, _exp, _act); \
        return 1; \
    } \
} while(0)

#define ASSERT_NE(not_expected, actual) do { \
    long long _nexp = (long long)(not_expected); \
    long long _act = (long long)(actual); \
    if (_nexp == _act) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_NE(%s, %s) - both are %lld" COLOR_RESET "\n", \
                __FILE__, __LINE__, #not_expected, #actual, _act); \
        return 1; \
    } \
} while(0)

#define ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_NULL(%s) failed - was %p" COLOR_RESET "\n", \
                __FILE__, __LINE__, #ptr, (void*)(ptr)); \
        return 1; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_NOT_NULL(%s) failed" COLOR_RESET "\n", \
                __FILE__, __LINE__, #ptr); \
        return 1; \
    } \
} while(0)

#define ASSERT_STR_EQ(expected, actual) do { \
    const char *_exp = (expected); \
    const char *_act = (actual); \
    if (_exp == NULL && _act == NULL) break; \
    if (_exp == NULL || _act == NULL || strcmp(_exp, _act) != 0) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_STR_EQ(%s, %s) - expected \"%s\", got \"%s\"" COLOR_RESET "\n", \
                __FILE__, __LINE__, #expected, #actual, _exp ? _exp : "(null)", _act ? _act : "(null)"); \
        return 1; \
    } \
} while(0)

#define ASSERT_MEM_EQ(expected, actual, size) do { \
    if (memcmp((expected), (actual), (size)) != 0) { \
        fprintf(stderr, COLOR_RED "  FAIL: %s:%d: ASSERT_MEM_EQ(%s, %s, %zu) failed" COLOR_RESET "\n", \
                __FILE__, __LINE__, #expected, #actual, (size_t)(size)); \
        return 1; \
    } \
} while(0)

/* Test definition macro - tests return 0 on success, non-zero on failure */
#define TEST(name) static int test_##name(void)

/* Run a single test */
#define RUN_TEST(name) do { \
    current_test_name = #name; \
    tests_run++; \
    fprintf(stderr, "  Running: %s... ", #name); \
    int result = test_##name(); \
    if (result == 0) { \
        tests_passed++; \
        fprintf(stderr, COLOR_GREEN "OK" COLOR_RESET "\n"); \
    } else { \
        tests_failed++; \
    } \
} while(0)

/* Test suite header */
#define TEST_SUITE_BEGIN(name) do { \
    fprintf(stderr, "\n" COLOR_YELLOW "=== %s ===" COLOR_RESET "\n", name); \
} while(0)

/* Print final summary */
#define TEST_SUMMARY() do { \
    fprintf(stderr, "\n" COLOR_YELLOW "=== Test Summary ===" COLOR_RESET "\n"); \
    fprintf(stderr, "  Total:  %d\n", tests_run); \
    fprintf(stderr, "  " COLOR_GREEN "Passed: %d" COLOR_RESET "\n", tests_passed); \
    if (tests_failed > 0) { \
        fprintf(stderr, "  " COLOR_RED "Failed: %d" COLOR_RESET "\n", tests_failed); \
    } else { \
        fprintf(stderr, "  Failed: %d\n", tests_failed); \
    } \
    fprintf(stderr, "\n"); \
} while(0)

/* Return exit code based on test results */
#define TEST_EXIT_CODE() (tests_failed > 0 ? 1 : 0)

#endif /* TEST_FRAMEWORK_H */
