/**
 * @file test_context.c
 * @brief Unit tests for FconcatContext API safety.
 */

#include "test_framework.h"
#include "../../src/core/context.h"
#include "../../src/output/output.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int context_test_sink_write(OutputSink *sink, const char *data, size_t size)
{
    (void)sink;
    (void)data;
    (void)size;
    return 0;
}

static void init_context_test_sink(OutputSink *sink)
{
    memset(sink, 0, sizeof(*sink));
    sink->write = context_test_sink_write;
}

static int context_join_path(char *buffer, size_t size, const char *root, const char *name)
{
    int n = snprintf(buffer, size, "%s/%s", root, name);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

static int context_write_test_file(const char *root, const char *name, const char *content)
{
    char path[MAX_PATH];
    if (context_join_path(path, sizeof(path), root, name) != 0)
        return -1;

    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;

    size_t len = strlen(content);
    int result = fwrite(content, 1, len, file) == len ? 0 : -1;
    if (fclose(file) != 0)
        result = -1;
    return result;
}

static void context_cleanup_test_root(const char *root)
{
    if (!root)
        return;

    char path[MAX_PATH];
    if (context_join_path(path, sizeof(path), root, "one.txt") == 0)
        unlink(path);
    rmdir(root);
}

static char *context_create_test_root(char *template, size_t template_size)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0')
        tmpdir = "/tmp";

    int n = snprintf(template, template_size, "%s/fconcat_context_XXXXXX", tmpdir);
    if (n < 0 || (size_t)n >= template_size)
        return NULL;

    char *root = mkdtemp(template);
    if (!root)
        return NULL;

    if (context_write_test_file(root, "one.txt", "one\n") != 0)
    {
        context_cleanup_test_root(root);
        return NULL;
    }

    return root;
}

TEST(create_fconcat_context_rejects_null_inputs)
{
    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    OutputSink sink;
    init_context_test_sink(&sink);

    ASSERT_NULL(create_fconcat_context(NULL, &sink, NULL, NULL, NULL, NULL));
    ASSERT_NULL(create_fconcat_context(&config, NULL, NULL, NULL, NULL, NULL));
    return 0;
}

TEST(create_fconcat_context_accepts_minimal_valid_inputs)
{
    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    config.output_file = "out.txt";

    OutputSink sink;
    init_context_test_sink(&sink);

    FconcatContext *ctx = create_fconcat_context(&config, &sink, NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(ctx);
    ASSERT_TRUE(ctx->config == &config);
    ASSERT_TRUE(((InternalContextState *)ctx->internal_state)->output_sink == &sink);

    destroy_fconcat_context(ctx);
    return 0;
}

TEST(context_progress_counters_saturate)
{
    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    config.output_file = "out.txt";

    OutputSink sink;
    init_context_test_sink(&sink);

    ProcessingStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.processed_bytes = SIZE_MAX - 5;

    FconcatContext *ctx = create_fconcat_context(&config, &sink, &stats, NULL, NULL, NULL);
    ASSERT_NOT_NULL(ctx);

    ctx->current_file_processed_bytes = SIZE_MAX - 3;
    update_context_progress(ctx, 10);

    ASSERT_TRUE(ctx->current_file_processed_bytes == SIZE_MAX);
    ASSERT_TRUE(stats.processed_bytes == SIZE_MAX);

    destroy_fconcat_context(ctx);
    return 0;
}

TEST(context_file_counter_saturates)
{
    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    config.output_file = "out.txt";

    OutputSink sink;
    init_context_test_sink(&sink);

    ProcessingStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.processed_files = SIZE_MAX;

    FconcatContext *ctx = create_fconcat_context(&config, &sink, &stats, NULL, NULL, NULL);
    ASSERT_NOT_NULL(ctx);

    FileInfo info;
    memset(&info, 0, sizeof(info));
    update_context_for_file(ctx, "file.txt", &info);

    ASSERT_TRUE(stats.processed_files == SIZE_MAX);

    destroy_fconcat_context(ctx);
    return 0;
}

TEST(process_fconcat_document_rejects_missing_callbacks)
{
    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    config.input_directory = "/definitely-not-a-fconcat-test-root";
    config.output_file = "out.txt";

    FconcatContext empty_ctx;
    memset(&empty_ctx, 0, sizeof(empty_ctx));
    ASSERT_EQ(-1, process_fconcat_document(&empty_ctx, &config, NULL, NULL));

    OutputSink sink;
    init_context_test_sink(&sink);
    FconcatContext partial_ctx;
    memset(&partial_ctx, 0, sizeof(partial_ctx));
    partial_ctx.write_output = context_write_output;
    partial_ctx.write_output_fmt = context_write_output_fmt;
    partial_ctx.get_config_bool = context_get_config_bool;
    ASSERT_EQ(-1, process_fconcat_document(&partial_ctx, &config, NULL, NULL));
    return 0;
}

TEST(process_fconcat_document_clears_index_owned_current_file)
{
    char root_template[MAX_PATH];
    char *root = context_create_test_root(root_template, sizeof(root_template));
    ASSERT_NOT_NULL(root);

    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    config.input_directory = root;
    config.output_file = "out.txt";
    config.binary_handling = BINARY_SKIP;
    config.symlink_handling = SYMLINK_SKIP;

    OutputSink sink;
    init_context_test_sink(&sink);

    FconcatContext *ctx = create_fconcat_context(&config, &sink, NULL, NULL, NULL, NULL);
    if (!ctx)
    {
        context_cleanup_test_root(root);
        return 1;
    }

    int result = process_fconcat_document(ctx, &config, NULL, NULL);
    const char *current_file_path = ctx->current_file_path;
    const void *current_file_info = ctx->current_file_info;
    size_t current_file_processed_bytes = ctx->current_file_processed_bytes;
    int current_directory_level = ctx->current_directory_level;

    destroy_fconcat_context(ctx);
    context_cleanup_test_root(root);

    ASSERT_EQ(0, result);
    ASSERT_NULL(current_file_path);
    ASSERT_NULL(current_file_info);
    ASSERT_EQ(0, current_file_processed_bytes);
    ASSERT_EQ(0, current_directory_level);
    return 0;
}

int test_context_main(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    TEST_SUITE_BEGIN("FconcatContext Safety");
    RUN_TEST(create_fconcat_context_rejects_null_inputs);
    RUN_TEST(create_fconcat_context_accepts_minimal_valid_inputs);
    RUN_TEST(context_progress_counters_saturate);
    RUN_TEST(context_file_counter_saturates);
    RUN_TEST(process_fconcat_document_rejects_missing_callbacks);
    RUN_TEST(process_fconcat_document_clears_index_owned_current_file);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
