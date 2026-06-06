/**
 * @file test_output.c
 * @brief Unit tests for output sink and text emission safety.
 */

#include "test_framework.h"
#include "../../src/core/context.h"
#include "../../src/output/output.h"
#include <stdarg.h>
#include <string.h>

typedef struct
{
    OutputSink sink;
    char data[2048];
    size_t used;
    int flush_count;
    int close_count;
    int fail_writes;
} TestOutputSink;

static int test_sink_write(OutputSink *sink, const char *data, size_t size)
{
    TestOutputSink *test = (TestOutputSink *)sink;
    if (!test || !data || test->fail_writes)
        return -1;
    if (size == 0)
        size = strlen(data);
    if (size > sizeof(test->data) - test->used)
        return -1;
    memcpy(test->data + test->used, data, size);
    test->used += size;
    return 0;
}

static int test_sink_flush(OutputSink *sink)
{
    TestOutputSink *test = (TestOutputSink *)sink;
    if (!test)
        return -1;
    test->flush_count++;
    return 0;
}

static int test_sink_close(OutputSink *sink)
{
    TestOutputSink *test = (TestOutputSink *)sink;
    if (!test)
        return -1;
    test->close_count++;
    return 0;
}

static int test_sink_fd(OutputSink *sink)
{
    (void)sink;
    return -1;
}

static void test_sink_init(TestOutputSink *sink)
{
    memset(sink, 0, sizeof(*sink));
    sink->sink.user_data = sink;
    sink->sink.write = test_sink_write;
    sink->sink.flush = test_sink_flush;
    sink->sink.close = test_sink_close;
    sink->sink.fd = test_sink_fd;
}

static int test_context_write_output(FconcatContext *ctx, const char *data, size_t size)
{
    if (!ctx || !ctx->internal_state)
        return -1;
    InternalContextState *internal = (InternalContextState *)ctx->internal_state;
    return output_sink_write(internal->output_sink, data, size);
}

static bool test_context_get_bool_false(FconcatContext *ctx, const char *key)
{
    (void)ctx;
    (void)key;
    return false;
}

static void test_context_init(FconcatContext *ctx, InternalContextState *internal, OutputSink *sink)
{
    memset(ctx, 0, sizeof(*ctx));
    memset(internal, 0, sizeof(*internal));
    internal->output_sink = sink;
    ctx->internal_state = internal;
    ctx->write_output = test_context_write_output;
    ctx->get_config_bool = test_context_get_bool_false;
}

TEST(output_sink_null_inputs_are_safe)
{
    ASSERT_EQ(-1, output_sink_write(NULL, "x", 1));
    ASSERT_EQ(-1, output_sink_write_cstr(NULL, "x"));
    ASSERT_EQ(-1, output_sink_write_fmt(NULL, "%s", "x"));
    ASSERT_EQ(-1, output_sink_flush(NULL));
    ASSERT_EQ(0, output_sink_close(NULL));
    ASSERT_EQ(-1, output_sink_fd(NULL));
    return 0;
}

TEST(buffered_sink_flushes_and_closes_downstream)
{
    TestOutputSink downstream;
    test_sink_init(&downstream);

    BufferedOutputSink buffered;
    ASSERT_EQ(0, buffered_output_sink_init(&buffered, &downstream.sink, 8, 1));

    int write_ok = output_sink_write(&buffered.sink, "hello", 5) == 0 &&
                   output_sink_write(&buffered.sink, " world", 6) == 0;
    int close_result = output_sink_close(&buffered.sink);
    int bytes_ok = downstream.used == strlen("hello world") &&
                   memcmp(downstream.data, "hello world", downstream.used) == 0;
    int close_count = downstream.close_count;

    buffered_output_sink_destroy(&buffered);

    ASSERT_TRUE(write_ok);
    ASSERT_EQ(0, close_result);
    ASSERT_TRUE(bytes_ok);
    ASSERT_EQ(1, close_count);
    return 0;
}

TEST(buffered_sink_rejects_zero_capacity_state)
{
    TestOutputSink downstream;
    test_sink_init(&downstream);

    BufferedOutputSink buffered;
    ASSERT_EQ(0, buffered_output_sink_init(&buffered, &downstream.sink, 8, 0));
    buffered.capacity = 0;

    int write_result = output_sink_write(&buffered.sink, "x", 1);
    buffered_output_sink_destroy(&buffered);

    ASSERT_EQ(-1, write_result);
    return 0;
}

TEST(text_output_null_guards)
{
    ASSERT_EQ(-1, text_begin_structure(NULL));
    ASSERT_EQ(-1, text_write_directory(NULL, "x", 0));
    ASSERT_EQ(-1, text_write_file_entry(NULL, "x", NULL));
    ASSERT_EQ(-1, text_begin_content(NULL));
    ASSERT_EQ(-1, text_write_file_header(NULL, "x"));
    ASSERT_EQ(-1, text_write_file_chunk(NULL, "x", 1));
    ASSERT_EQ(-1, text_write_file_footer(NULL));
    ASSERT_EQ(-1, text_write_binary_placeholder(NULL));
    ASSERT_EQ(-1, text_write_symlink_placeholder(NULL, "x"));
    ASSERT_EQ(-1, text_end_document(NULL));
    return 0;
}

TEST(text_output_rejects_invalid_levels)
{
    TestOutputSink sink;
    test_sink_init(&sink);
    InternalContextState internal;
    FconcatContext ctx;
    test_context_init(&ctx, &internal, &sink.sink);

    ASSERT_EQ(-1, text_write_directory(&ctx, "bad", -1));
    ctx.current_directory_level = -1;
    ASSERT_EQ(-1, text_write_file_entry(&ctx, "bad", NULL));
    return 0;
}

TEST(text_output_writes_expected_structure)
{
    TestOutputSink sink;
    test_sink_init(&sink);
    InternalContextState internal;
    FconcatContext ctx;
    test_context_init(&ctx, &internal, &sink.sink);

    ASSERT_EQ(0, text_begin_structure(&ctx));
    ASSERT_EQ(0, text_write_directory(&ctx, "src", 1));
    ctx.current_directory_level = 1;
    ASSERT_EQ(0, text_write_file_entry(&ctx, "src/main.c", NULL));
    ASSERT_EQ(0, text_end_document(&ctx));

    ASSERT_TRUE(sink.flush_count > 0);
    ASSERT_TRUE(strstr(sink.data, "Directory Structure:") != NULL);
    ASSERT_TRUE(strstr(sink.data, "  DIR  src/") != NULL);
    ASSERT_TRUE(strstr(sink.data, "  FILE src/main.c") != NULL);
    return 0;
}

int test_output_main(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    TEST_SUITE_BEGIN("OutputSink Safety");
    RUN_TEST(output_sink_null_inputs_are_safe);
    RUN_TEST(buffered_sink_flushes_and_closes_downstream);
    RUN_TEST(buffered_sink_rejects_zero_capacity_state);

    TEST_SUITE_BEGIN("Text Output Safety");
    RUN_TEST(text_output_null_guards);
    RUN_TEST(text_output_rejects_invalid_levels);
    RUN_TEST(text_output_writes_expected_structure);

    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
