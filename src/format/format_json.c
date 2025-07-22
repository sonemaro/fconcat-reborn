#include "format.h"
#include <stdio.h>
#include <string.h>

// JSON formatter state
typedef struct
{
    bool first_file;
    bool first_dir;
    bool in_content_section;
} JsonState;

static JsonState json_state = {0};

static int json_begin_document(FconcatContext *ctx)
{
    json_state.first_file = true;
    json_state.first_dir = true;
    json_state.in_content_section = false;
    return ctx->write_output(ctx, "{\n", 2);
}

static int json_begin_structure(FconcatContext *ctx)
{
    ctx->write_output(ctx, "  \"structure\": {\n", 0);
    ctx->write_output(ctx, "    \"directories\": [\n", 0);
    json_state.first_dir = true;
    return 0;
}

static int json_write_directory(FconcatContext *ctx, const char *path, int level)
{
    if (!json_state.first_dir)
    {
        ctx->write_output(ctx, ",\n", 2);
    }
    json_state.first_dir = false;

    ctx->write_output(ctx, "      {\n", 0);
    ctx->write_output(ctx, "        \"path\": \"", 0);
    ctx->write_output(ctx, path, 0);
    ctx->write_output(ctx, "\",\n", 0);
    ctx->write_output_fmt(ctx, "        \"level\": %d\n", level);
    ctx->write_output(ctx, "      }", 0);
    return 0;
}

static int json_write_file_entry(FconcatContext *ctx, const char *path, void *info)
{
    if (!json_state.first_file)
    {
        ctx->write_output(ctx, ",\n", 2);
    }
    json_state.first_file = false;

    ctx->write_output(ctx, "      {\n", 0);
    ctx->write_output(ctx, "        \"path\": \"", 0);
    ctx->write_output(ctx, path, 0);
    ctx->write_output(ctx, "\",\n", 0);
    ctx->write_output_fmt(ctx, "        \"level\": %d", ctx->current_directory_level);

    if (info)
    {
        FileInfo *file_info = (FileInfo *)info;
        ctx->write_output(ctx, ",\n", 0);
        ctx->write_output_fmt(ctx, "        \"size\": %zu,\n", file_info->size);
        ctx->write_output_fmt(ctx, "        \"is_binary\": %s,\n", file_info->is_binary ? "true" : "false");
        ctx->write_output_fmt(ctx, "        \"is_symlink\": %s", file_info->is_symlink ? "true" : "false");
    }

    ctx->write_output(ctx, "\n      }", 0);
    return 0;
}

static int json_end_structure(FconcatContext *ctx)
{
    ctx->write_output(ctx, "\n    ],\n", 0);
    ctx->write_output(ctx, "    \"files\": [\n", 0);
    json_state.first_file = true;
    return 0;
}

static int json_begin_content(FconcatContext *ctx)
{
    ctx->write_output(ctx, "\n    ]\n", 0);
    ctx->write_output(ctx, "  },\n", 0);
    ctx->write_output(ctx, "  \"contents\": [\n", 0);
    json_state.first_file = true;
    json_state.in_content_section = true;
    return 0;
}

static int json_write_file_header(FconcatContext *ctx, const char *path)
{
    if (!json_state.first_file)
    {
        ctx->write_output(ctx, ",\n", 2);
    }
    json_state.first_file = false;

    ctx->write_output(ctx, "    {\n", 0);
    ctx->write_output(ctx, "      \"path\": \"", 0);
    ctx->write_output(ctx, path, 0);
    ctx->write_output(ctx, "\",\n", 0);
    ctx->write_output(ctx, "      \"content\": \"", 0);
    return 0;
}

static int json_write_file_chunk(FconcatContext *ctx, const char *data, size_t size)
{
    // JSON escape the data
    for (size_t i = 0; i < size; i++)
    {
        char c = data[i];
        switch (c)
        {
        case '"':
            ctx->write_output(ctx, "\\\"", 2);
            break;
        case '\\':
            ctx->write_output(ctx, "\\\\", 2);
            break;
        case '\n':
            ctx->write_output(ctx, "\\n", 2);
            break;
        case '\r':
            ctx->write_output(ctx, "\\r", 2);
            break;
        case '\t':
            ctx->write_output(ctx, "\\t", 2);
            break;
        default:
            ctx->write_output(ctx, &c, 1);
            break;
        }
    }
    return 0;
}

static int json_write_file_footer(FconcatContext *ctx)
{
    ctx->write_output(ctx, "\"\n    }", 0);
    return 0;
}

static int json_end_content(FconcatContext *ctx)
{
    ctx->write_output(ctx, "\n  ]\n", 0);
    return 0;
}

static int json_end_document(FconcatContext *ctx)
{
    ctx->write_output(ctx, "}\n", 2);
    return 0;
}

static FormatPlugin json_plugin = {
    .name = "json",
    .file_extension = "json",
    .mime_type = "application/json",
    .init = NULL,
    .begin_document = json_begin_document,
    .begin_structure = json_begin_structure,
    .write_directory = json_write_directory,
    .write_file_entry = json_write_file_entry,
    .end_structure = json_end_structure,
    .begin_content = json_begin_content,
    .write_file_header = json_write_file_header,
    .write_file_chunk = json_write_file_chunk,
    .write_file_footer = json_write_file_footer,
    .end_content = json_end_content,
    .end_document = json_end_document,
    .cleanup = NULL};

FormatPlugin *format_json_plugin(void)
{
    return &json_plugin;
}