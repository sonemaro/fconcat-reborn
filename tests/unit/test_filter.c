/**
 * @file test_filter.c
 * @brief Unit tests for filter subsystem
 * 
 * Tests cover:
 * - FilterEngine lifecycle (create/destroy)
 * - Binary file detection
 * - Path utility functions
 * - Exclude pattern matching
 * - Include pattern matching
 */

#include "test_framework.h"
#include "../../src/filter/filter.h"
#include "../../src/filter/filter_utils.h"
#include "../../src/config/config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

/* =========================================================================
 * Test Helpers
 * ========================================================================= */

static char test_temp_dir[256] = {0};
static int temp_dir_created = 0;

static void create_test_temp_dir(void)
{
    if (temp_dir_created) return;
    const char *base = getenv("TMPDIR");
    if (!base || base[0] == '\0')
        base = getenv("HOME");
    if (!base || base[0] == '\0')
        base = ".";
    snprintf(test_temp_dir, sizeof(test_temp_dir), "%s/fconcat_test_%d", base, getpid());
    mkdir(test_temp_dir, 0755);
    temp_dir_created = 1;
}

static void cleanup_test_temp_dir(void)
{
    if (!temp_dir_created) return;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_temp_dir);
    int result = system(cmd);
    (void)result;  // Intentionally ignore - cleanup is best effort
    temp_dir_created = 0;
}

static char* create_test_file(const char *filename, const char *content, size_t len)
{
    create_test_temp_dir();
    static char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", test_temp_dir, filename);
    
    FILE *f = fopen(filepath, "wb");
    if (!f) return NULL;
    fwrite(content, 1, len, f);
    fclose(f);
    return filepath;
}

/* =========================================================================
 * FilterEngine Lifecycle Tests
 * ========================================================================= */

TEST(filter_engine_create_returns_valid_pointer)
{
    FilterEngine *engine = filter_engine_create();
    ASSERT_NOT_NULL(engine);
    filter_engine_destroy(engine);
    return 0;
}

TEST(filter_engine_destroy_null_is_safe)
{
    filter_engine_destroy(NULL);
    return 0;
}

TEST(filter_engine_initial_state)
{
    FilterEngine *engine = filter_engine_create();
    ASSERT_NOT_NULL(engine);
    ASSERT_NOT_NULL(engine->rules);
    ASSERT_EQ(0, engine->rule_count);
    filter_engine_destroy(engine);
    return 0;
}

/* =========================================================================
 * Path Utility Tests
 * ========================================================================= */

TEST(get_filename_util_basic)
{
    ASSERT_STR_EQ("file.txt", get_filename_util("/path/to/file.txt"));
    ASSERT_STR_EQ("file.txt", get_filename_util("file.txt"));
    ASSERT_STR_EQ("file", get_filename_util("/path/to/file"));
    return 0;
}

TEST(get_filename_util_null_safe)
{
    ASSERT_NULL(get_filename_util(NULL));
    return 0;
}

TEST(get_filename_util_edge_cases)
{
    ASSERT_STR_EQ("file", get_filename_util("path/to/file"));
    ASSERT_STR_EQ("", get_filename_util("/path/to/"));  // Empty after last /
    return 0;
}

TEST(filter_get_basename_mixed_separators)
{
    ASSERT_STR_EQ("file.txt", filter_get_basename("/unix/path/file.txt"));
    ASSERT_STR_EQ("file.txt", filter_get_basename("C:\\windows\\path\\file.txt"));
    ASSERT_STR_EQ("last.txt", filter_get_basename("/mixed\\path/last.txt"));
    ASSERT_STR_EQ("last.txt", filter_get_basename("\\mixed/path\\last.txt"));
    ASSERT_NULL(filter_get_basename(NULL));
    return 0;
}

TEST(get_absolute_path_util_null_safe)
{
    ASSERT_NULL(get_absolute_path_util(NULL));
    return 0;
}

TEST(get_absolute_path_util_returns_path)
{
    char *path = get_absolute_path_util("/tmp");
    ASSERT_NOT_NULL(path);
    ASSERT_TRUE(path[0] == '/');  // Should be absolute
    free(path);
    return 0;
}

/* =========================================================================
 * Binary Detection Tests
 * ========================================================================= */

TEST(filter_is_binary_null_safe)
{
    ASSERT_EQ(-1, filter_is_binary_file(NULL));
    return 0;
}

TEST(filter_is_binary_nonexistent_file)
{
    ASSERT_EQ(-1, filter_is_binary_file("/nonexistent/file/path"));
    return 0;
}

TEST(filter_is_binary_text_file)
{
    const char *content = "Hello, World!\nThis is a text file.\n";
    char *filepath = create_test_file("text.txt", content, strlen(content));
    ASSERT_NOT_NULL(filepath);
    
    int result = filter_is_binary_file(filepath);
    ASSERT_EQ(0, result);  // 0 = text
    return 0;
}

TEST(filter_is_binary_binary_file)
{
    // Create a file with null bytes (binary indicator)
    unsigned char content[] = {'H', 'e', 'l', 'l', 'o', 0, 'W', 'o', 'r', 'l', 'd'};
    char *filepath = create_test_file("binary.bin", (char*)content, sizeof(content));
    ASSERT_NOT_NULL(filepath);
    
    int result = filter_is_binary_file(filepath);
    ASSERT_EQ(1, result);  // 1 = binary
    return 0;
}

TEST(filter_is_binary_empty_file)
{
    char *filepath = create_test_file("empty.txt", "", 0);
    ASSERT_NOT_NULL(filepath);
    
    int result = filter_is_binary_file(filepath);
    ASSERT_EQ(0, result);  // Empty file is text
    return 0;
}

TEST(filter_is_binary_unicode_text)
{
    // UTF-8 text with multi-byte characters
    const char *content = "Hello, 世界! Привет мир! 🌍";
    char *filepath = create_test_file("unicode.txt", content, strlen(content));
    ASSERT_NOT_NULL(filepath);
    
    int result = filter_is_binary_file(filepath);
    ASSERT_EQ(0, result);  // UTF-8 has no null bytes
    return 0;
}

/* =========================================================================
 * Exclude Pattern Tests
 * Note: exclude_match_path returns:
 *   1 = pattern matches (should EXCLUDE the file)
 *   0 = no match (should INCLUDE the file)
 * ========================================================================= */

TEST(exclude_match_path_basic_pattern)
{
    ExcludeContext ctx;
    char *patterns[] = {"*.log", "*.tmp"};
    ctx.patterns = patterns;
    ctx.pattern_count = 2;
    
    // Should match *.log pattern - returns 1 meaning "exclude this"
    int result = exclude_match_path("debug.log", NULL, &ctx);
    ASSERT_EQ(1, result);  // 1 = matches exclusion pattern
    
    // Should not match any pattern - returns 0 meaning "don't exclude"
    result = exclude_match_path("source.c", NULL, &ctx);
    ASSERT_EQ(0, result);  // 0 = no match
    
    return 0;
}

TEST(exclude_match_path_directory_pattern)
{
    ExcludeContext ctx;
    char *patterns[] = {"node_modules", ".git"};
    ctx.patterns = patterns;
    ctx.pattern_count = 2;
    
    // Direct match - returns 1 meaning "exclude this"
    int result = exclude_match_path("node_modules", NULL, &ctx);
    ASSERT_EQ(1, result);  // matches
    
    // Path containing pattern in basename
    result = exclude_match_path("project/node_modules", NULL, &ctx);
    ASSERT_EQ(1, result);  // matches (basename is node_modules)
    
    return 0;
}

TEST(exclude_match_path_null_context)
{
    // Null context returns 0 (no exclusion patterns means nothing matches)
    int result = exclude_match_path("file.txt", NULL, NULL);
    ASSERT_EQ(0, result);  // 0 = don't exclude
    return 0;
}

TEST(exclude_match_path_rejects_corrupt_context_counts)
{
    ExcludeContext ctx;
    char *patterns[] = {"*.nomatch"};
    ctx.patterns = patterns;

    ctx.pattern_count = -1;
    ASSERT_EQ(0, exclude_match_path("debug.log", NULL, &ctx));

    ctx.pattern_count = MAX_EXCLUDES + 1;
    ASSERT_EQ(0, exclude_match_path("debug.log", NULL, &ctx));

    ctx.pattern_count = 1;
    ctx.patterns = NULL;
    ASSERT_EQ(0, exclude_match_path("debug.log", NULL, &ctx));
    return 0;
}

/* =========================================================================
 * Include Pattern Tests  
 * Note: include_match_path returns:
 *   1 = pattern matches (should INCLUDE the file)
 *   0 = no match (should EXCLUDE when include patterns active)
 * ========================================================================= */

TEST(include_match_path_basic_pattern)
{
    IncludeContext ctx;
    char *patterns[] = {"*.c", "*.h"};
    ctx.patterns = patterns;
    ctx.pattern_count = 2;
    
    // Should match *.c pattern
    int result = include_match_path("main.c", NULL, &ctx);
    ASSERT_EQ(1, result);  // 1 = include
    
    // Should not match any pattern
    result = include_match_path("readme.md", NULL, &ctx);
    ASSERT_EQ(0, result);  // 0 = no match
    
    return 0;
}

TEST(include_match_path_null_context)
{
    // Null context returns 0 (implementation returns 0 when ctx is NULL)
    int result = include_match_path("file.txt", NULL, NULL);
    ASSERT_EQ(0, result);
    return 0;
}

TEST(include_match_path_empty_patterns)
{
    IncludeContext ctx;
    ctx.patterns = NULL;
    ctx.pattern_count = 0;
    
    // No patterns means the loop doesn't execute - returns 0
    int result = include_match_path("anything.xyz", NULL, &ctx);
    ASSERT_EQ(0, result);
    return 0;
}

TEST(include_match_path_rejects_corrupt_context_counts)
{
    IncludeContext ctx;
    char *patterns[] = {"*.nomatch"};
    ctx.patterns = patterns;

    ctx.pattern_count = -1;
    ASSERT_EQ(0, include_match_path("main.c", NULL, &ctx));

    ctx.pattern_count = MAX_INCLUDES + 1;
    ASSERT_EQ(0, include_match_path("main.c", NULL, &ctx));

    ctx.pattern_count = 1;
    ctx.patterns = NULL;
    ASSERT_EQ(0, include_match_path("main.c", NULL, &ctx));
    return 0;
}

/* =========================================================================
 * Filter Rule Tests
 * ========================================================================= */

TEST(filter_engine_add_rule)
{
    FilterEngine *engine = filter_engine_create();
    ASSERT_NOT_NULL(engine);
    
    FilterRule rule = {0};
    rule.type = FILTER_TYPE_EXCLUDE;
    rule.priority = 10;
    
    int result = filter_engine_add_rule_internal(engine, &rule);
    ASSERT_EQ(0, result);
    ASSERT_EQ(1, engine->rule_count);
    
    filter_engine_destroy(engine);
    return 0;
}

TEST(filter_engine_add_multiple_rules)
{
    FilterEngine *engine = filter_engine_create();
    ASSERT_NOT_NULL(engine);
    
    for (int i = 0; i < 125; i++) {
        FilterRule rule = {0};
        rule.type = FILTER_TYPE_EXCLUDE;
        rule.priority = i;
        
        int result = filter_engine_add_rule_internal(engine, &rule);
        ASSERT_EQ(0, result);
    }
    
    ASSERT_EQ(125, engine->rule_count);
    ASSERT_TRUE(engine->rule_capacity >= 125);
    
    filter_engine_destroy(engine);
    return 0;
}

TEST(filter_engine_add_rule_rejects_corrupt_state)
{
    FilterEngine *engine = filter_engine_create();
    ASSERT_NOT_NULL(engine);

    FilterRule rule = {0};
    rule.type = FILTER_TYPE_EXCLUDE;
    int saved_capacity = engine->rule_capacity;
    FilterRule *saved_rules = engine->rules;

    engine->rule_count = -1;
    ASSERT_EQ(-1, filter_engine_add_rule_internal(engine, &rule));
    engine->rule_count = 0;

    engine->rule_capacity = 0;
    ASSERT_EQ(-1, filter_engine_add_rule_internal(engine, &rule));
    engine->rule_capacity = saved_capacity;

    engine->rule_count = saved_capacity + 1;
    ASSERT_EQ(-1, filter_engine_add_rule_internal(engine, &rule));
    engine->rule_count = 0;

    engine->rules = NULL;
    ASSERT_EQ(-1, filter_engine_add_rule_internal(engine, &rule));
    engine->rules = saved_rules;

    engine->rule_count = INT_MAX;
    engine->rule_capacity = INT_MAX;
    ASSERT_EQ(-1, filter_engine_add_rule_internal(engine, &rule));
    engine->rule_count = 0;
    engine->rule_capacity = saved_capacity;

    filter_engine_destroy(engine);
    return 0;
}

TEST(filter_pattern_initializers_reject_invalid_counts)
{
    FilterEngine *engine = filter_engine_create();
    ASSERT_NOT_NULL(engine);

    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    char *patterns[] = {"*.c"};

    config.include_count = -1;
    ASSERT_EQ(-1, filter_include_patterns_init_internal(engine, &config));
    ASSERT_EQ(0, engine->rule_count);

    config.include_count = 1;
    config.include_patterns = NULL;
    ASSERT_EQ(-1, filter_include_patterns_init_internal(engine, &config));
    ASSERT_EQ(0, engine->rule_count);

    config.include_count = MAX_INCLUDES + 1;
    config.include_patterns = patterns;
    ASSERT_EQ(-1, filter_include_patterns_init_internal(engine, &config));
    ASSERT_EQ(0, engine->rule_count);

    memset(&config, 0, sizeof(config));
    config.exclude_count = -1;
    ASSERT_EQ(-1, filter_exclude_patterns_init_internal(engine, &config));
    ASSERT_EQ(0, engine->rule_count);

    config.exclude_count = 1;
    config.exclude_patterns = NULL;
    ASSERT_EQ(-1, filter_exclude_patterns_init_internal(engine, &config));
    ASSERT_EQ(0, engine->rule_count);

    config.exclude_count = MAX_EXCLUDES + 1;
    config.exclude_patterns = patterns;
    ASSERT_EQ(-1, filter_exclude_patterns_init_internal(engine, &config));
    ASSERT_EQ(0, engine->rule_count);

    filter_engine_destroy(engine);
    return 0;
}

TEST(filter_context_destroyers_handle_corrupt_counts)
{
    ExcludeContext *exclude_ctx = calloc(1, sizeof(*exclude_ctx));
    if (!exclude_ctx)
        return 1;
    exclude_ctx->patterns = calloc(1, sizeof(*exclude_ctx->patterns));
    if (!exclude_ctx->patterns)
    {
        destroy_exclude_context_wrapper(exclude_ctx);
        return 1;
    }
    exclude_ctx->pattern_count = MAX_EXCLUDES + 1;
    destroy_exclude_context_wrapper(exclude_ctx);

    IncludeContext *include_ctx = calloc(1, sizeof(*include_ctx));
    if (!include_ctx)
        return 1;
    include_ctx->patterns = calloc(1, sizeof(*include_ctx->patterns));
    if (!include_ctx->patterns)
    {
        destroy_include_context_wrapper(include_ctx);
        return 1;
    }
    include_ctx->pattern_count = -1;
    destroy_include_context_wrapper(include_ctx);

    exclude_ctx = calloc(1, sizeof(*exclude_ctx));
    if (!exclude_ctx)
        return 1;
    exclude_ctx->pattern_count = 1;
    exclude_ctx->patterns = NULL;
    destroy_exclude_context_wrapper(exclude_ctx);

    include_ctx = calloc(1, sizeof(*include_ctx));
    if (!include_ctx)
        return 1;
    include_ctx->pattern_count = MAX_INCLUDES + 1;
    include_ctx->patterns = NULL;
    destroy_include_context_wrapper(include_ctx);
    return 0;
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

int test_filter_main(void)
{
    /* Reset counters for this test suite */
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
    
    TEST_SUITE_BEGIN("FilterEngine Lifecycle");
    RUN_TEST(filter_engine_create_returns_valid_pointer);
    RUN_TEST(filter_engine_destroy_null_is_safe);
    RUN_TEST(filter_engine_initial_state);
    
    TEST_SUITE_BEGIN("Path Utilities");
    RUN_TEST(get_filename_util_basic);
    RUN_TEST(get_filename_util_null_safe);
    RUN_TEST(get_filename_util_edge_cases);
    RUN_TEST(filter_get_basename_mixed_separators);
    RUN_TEST(get_absolute_path_util_null_safe);
    RUN_TEST(get_absolute_path_util_returns_path);
    
    TEST_SUITE_BEGIN("Binary Detection");
    RUN_TEST(filter_is_binary_null_safe);
    RUN_TEST(filter_is_binary_nonexistent_file);
    RUN_TEST(filter_is_binary_text_file);
    RUN_TEST(filter_is_binary_binary_file);
    RUN_TEST(filter_is_binary_empty_file);
    RUN_TEST(filter_is_binary_unicode_text);
    
    TEST_SUITE_BEGIN("Exclude Patterns");
    RUN_TEST(exclude_match_path_basic_pattern);
    RUN_TEST(exclude_match_path_directory_pattern);
    RUN_TEST(exclude_match_path_null_context);
    RUN_TEST(exclude_match_path_rejects_corrupt_context_counts);
    
    TEST_SUITE_BEGIN("Include Patterns");
    RUN_TEST(include_match_path_basic_pattern);
    RUN_TEST(include_match_path_null_context);
    RUN_TEST(include_match_path_empty_patterns);
    RUN_TEST(include_match_path_rejects_corrupt_context_counts);
    
    TEST_SUITE_BEGIN("Filter Rules");
    RUN_TEST(filter_engine_add_rule);
    RUN_TEST(filter_engine_add_multiple_rules);
    RUN_TEST(filter_engine_add_rule_rejects_corrupt_state);
    RUN_TEST(filter_pattern_initializers_reject_invalid_counts);
    RUN_TEST(filter_context_destroyers_handle_corrupt_counts);
    
    TEST_SUMMARY();
    
    /* Cleanup temporary files */
    cleanup_test_temp_dir();
    
    return TEST_EXIT_CODE();
}
