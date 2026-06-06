/**
 * @file test_file_index.c
 * @brief Unit tests for FileIndex public API safety.
 */

#include "test_framework.h"
#include "../../src/core/context.h"
#include "../../src/core/file_index.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef FCONCAT_LEAK_GUARD
extern int file_index_test_dir_stack_destroy_corrupt_size(void);
extern int file_index_test_dir_stack_rejects_corrupt_state(void);
#endif

static int file_index_test_sink_write(OutputSink *sink, const char *data, size_t size)
{
    (void)sink;
    (void)data;
    (void)size;
    return 0;
}

static void file_index_test_sink_init(OutputSink *sink)
{
    memset(sink, 0, sizeof(*sink));
    sink->write = file_index_test_sink_write;
}

static int join_path(char *buffer, size_t size, const char *root, const char *name)
{
    int n = snprintf(buffer, size, "%s/%s", root, name);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

static int write_test_file(const char *root, const char *name, const char *content)
{
    char path[MAX_PATH];
    if (join_path(path, sizeof(path), root, name) != 0)
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

static void cleanup_simple_tree(const char *root)
{
    if (!root)
        return;

    char path[MAX_PATH];
    if (join_path(path, sizeof(path), root, "a.txt") == 0)
        unlink(path);
    if (join_path(path, sizeof(path), root, "b.txt") == 0)
        unlink(path);
    if (join_path(path, sizeof(path), root, "sub/nested.txt") == 0)
        unlink(path);
    if (join_path(path, sizeof(path), root, "sub") == 0)
        rmdir(path);
    rmdir(root);
}

static char *create_simple_tree(char *template, size_t template_size)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0')
        tmpdir = "/tmp";

    int n = snprintf(template, template_size, "%s/fconcat_file_index_XXXXXX", tmpdir);
    if (n < 0 || (size_t)n >= template_size)
        return NULL;

    char *root = mkdtemp(template);
    if (!root)
        return NULL;

    char subdir[MAX_PATH];
    if (join_path(subdir, sizeof(subdir), root, "sub") != 0 ||
        mkdir(subdir, 0755) != 0 ||
        write_test_file(root, "b.txt", "b\n") != 0 ||
        write_test_file(root, "a.txt", "a\n") != 0 ||
        write_test_file(root, "sub/nested.txt", "nested\n") != 0)
    {
        cleanup_simple_tree(root);
        return NULL;
    }

    return root;
}

TEST(file_index_accessors_handle_null)
{
    ASSERT_EQ(0, file_index_count(NULL));
    ASSERT_NULL(file_index_entry(NULL, 0));
    ASSERT_EQ(0, file_index_prefix_used(NULL));
    ASSERT_EQ(0, file_index_prefix_budget(NULL));
    file_index_destroy(NULL);
    return 0;
}

TEST(file_index_create_initializes_empty_index)
{
    FileIndex *index = file_index_create(1234);
    ASSERT_NOT_NULL(index);

    size_t count = file_index_count(index);
    size_t prefix_used = file_index_prefix_used(index);
    size_t prefix_budget = file_index_prefix_budget(index);
    FileIndexEntry *first = file_index_entry(index, 0);

    file_index_destroy(index);

    ASSERT_EQ(0, count);
    ASSERT_EQ(0, prefix_used);
    ASSERT_EQ(1234, prefix_budget);
    ASSERT_NULL(first);
    return 0;
}

TEST(file_index_build_rejects_null_inputs)
{
    FileIndex *index = file_index_create(0);
    ASSERT_NOT_NULL(index);

    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    config.input_directory = ".";

    OutputSink sink;
    file_index_test_sink_init(&sink);
    FconcatContext *ctx = create_fconcat_context(&config, &sink, NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(ctx);

    ASSERT_EQ(-1, file_index_build(NULL, ctx, &config, NULL, NULL, NULL));
    ASSERT_EQ(-1, file_index_build(index, NULL, &config, NULL, NULL, NULL));
    ASSERT_EQ(-1, file_index_build(index, ctx, NULL, NULL, NULL, NULL));

    config.input_directory = NULL;
    ASSERT_EQ(-1, file_index_build(index, ctx, &config, NULL, NULL, NULL));

    destroy_fconcat_context(ctx);
    file_index_destroy(index);
    return 0;
}

TEST(file_index_build_indexes_sorted_tree)
{
    char template[MAX_PATH];
    char *root = create_simple_tree(template, sizeof(template));
    ASSERT_NOT_NULL(root);

    ResolvedConfig config;
    memset(&config, 0, sizeof(config));
    config.input_directory = root;
    config.output_file = "out.txt";
    config.binary_handling = BINARY_INCLUDE;
    config.symlink_handling = SYMLINK_SKIP;

    OutputSink sink;
    file_index_test_sink_init(&sink);
    FconcatContext *ctx = create_fconcat_context(&config, &sink, NULL, NULL, NULL, NULL);
    ASSERT_NOT_NULL(ctx);

    FileIndex *index = file_index_create(0);
    ASSERT_NOT_NULL(index);

    int build_result = file_index_build(index, ctx, &config, NULL, NULL, NULL);
    size_t count = file_index_count(index);
    FileIndexEntry *entry0 = file_index_entry(index, 0);
    FileIndexEntry *entry1 = file_index_entry(index, 1);
    FileIndexEntry *entry2 = file_index_entry(index, 2);
    FileIndexEntry *entry3 = file_index_entry(index, 3);
    FileIndexEntry *entry4 = file_index_entry(index, 4);
    int names_ok = entry0 && entry1 && entry2 && entry3 && !entry4 &&
                   strcmp(entry0->relative_path, "a.txt") == 0 &&
                   strcmp(entry1->relative_path, "b.txt") == 0 &&
                   strcmp(entry2->relative_path, "sub") == 0 &&
                   strcmp(entry3->relative_path, "sub/nested.txt") == 0 &&
                   entry2->info.is_directory &&
                   !entry3->info.is_directory;

    file_index_destroy(index);
    destroy_fconcat_context(ctx);
    cleanup_simple_tree(root);

    ASSERT_EQ(0, build_result);
    ASSERT_EQ(4, count);
    ASSERT_TRUE(names_ok);
    return 0;
}

TEST(file_index_dir_stack_handles_corrupt_state)
{
#ifndef FCONCAT_LEAK_GUARD
    return 0;
#else
    ASSERT_EQ(0, file_index_test_dir_stack_destroy_corrupt_size());
    ASSERT_EQ(0, file_index_test_dir_stack_rejects_corrupt_state());
    return 0;
#endif
}

int test_file_index_main(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    TEST_SUITE_BEGIN("FileIndex Safety");
    RUN_TEST(file_index_accessors_handle_null);
    RUN_TEST(file_index_create_initializes_empty_index);
    RUN_TEST(file_index_build_rejects_null_inputs);
    RUN_TEST(file_index_build_indexes_sorted_tree);
    RUN_TEST(file_index_dir_stack_handles_corrupt_state);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
