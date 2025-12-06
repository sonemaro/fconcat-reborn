#include "format.h"
#include <stdio.h>
#include <string.h>

// Text formatter - implements the current fconcat output format
static int text_begin_document(FconcatContext *ctx)
{
    (void)ctx;
    return 0;
}

static int text_begin_structure(FconcatContext *ctx)
{
    return ctx->write_output(ctx, "Directory Structure:\n==================\n\n", 0);
}

static int text_write_directory(FconcatContext *ctx, const char *path, int level)
{
    int ret;
    for (int i = 0; i < level * 2; i++)
    {
        ret = ctx->write_output(ctx, " ", 1);
        if (ret != 0) return ret;
    }
    ret = ctx->write_output(ctx, "📁 ", 0);
    if (ret != 0) return ret;
    ret = ctx->write_output(ctx, path, 0);
    if (ret != 0) return ret;
    return ctx->write_output(ctx, "/\n", 2);
}

static int text_write_file_entry(FconcatContext *ctx, const char *path, void *info)
{
    int ret;
    for (int i = 0; i < ctx->current_directory_level * 2; i++)
    {
        ret = ctx->write_output(ctx, " ", 1);
        if (ret != 0) return ret;
    }

    ret = ctx->write_output(ctx, "📄 ", 0);
    if (ret != 0) return ret;

    // Use context configuration access functions
    bool show_size = ctx->get_config_bool(ctx, "show_size");
    if (show_size && info)
    {
        // Cast opaque pointer to FileInfo
        FileInfo *file_info = (FileInfo *)info;
        char size_buf[64];
        // Convert bytes to KB (round up)
        size_t kb = (file_info->size + 1023) / 1024;
        if (kb == 0 && file_info->size > 0)
            kb = 1; // At least 1 KB for non-empty files
        int len = snprintf(size_buf, sizeof(size_buf), "[%zu KB] ", kb);
        if (len > 0 && len < (int)sizeof(size_buf))
        {
            ret = ctx->write_output(ctx, size_buf, 0);
            if (ret != 0) return ret;
        }
    }

    ret = ctx->write_output(ctx, path, 0);
    if (ret != 0) return ret;
    return ctx->write_output(ctx, "\n", 1);
}

static int text_end_structure(FconcatContext *ctx)
{
    (void)ctx;
    return 0;
}

static int text_begin_content(FconcatContext *ctx)
{
    return ctx->write_output(ctx, "\nFile Contents:\n=============\n\n", 0);
}

static int text_write_file_header(FconcatContext *ctx, const char *path)
{
    int ret;
    ret = ctx->write_output(ctx, "// File: ", 0);
    if (ret != 0) return ret;
    ret = ctx->write_output(ctx, path, 0);
    if (ret != 0) return ret;
    return ctx->write_output(ctx, "\n", 1);
}

static int text_write_file_chunk(FconcatContext *ctx, const char *data, size_t size)
{
    return ctx->write_output(ctx, data, size);
}

static int text_write_file_footer(FconcatContext *ctx)
{
    return ctx->write_output(ctx, "\n\n", 2);
}

static int text_end_content(FconcatContext *ctx)
{
    (void)ctx;
    return 0;
}

static int text_end_document(FconcatContext *ctx)
{
    (void)ctx;
    return 0;
}

static FormatPlugin text_plugin = {
    .name = "text",
    .file_extension = "txt",
    .mime_type = "text/plain",
    .init = NULL,
    .begin_document = text_begin_document,
    .begin_structure = text_begin_structure,
    .write_directory = text_write_directory,
    .write_file_entry = text_write_file_entry,
    .end_structure = text_end_structure,
    .begin_content = text_begin_content,
    .write_file_header = text_write_file_header,
    .write_file_chunk = text_write_file_chunk,
    .write_file_footer = text_write_file_footer,
    .end_content = text_end_content,
    .end_document = text_end_document,
    .cleanup = NULL};

FormatPlugin *format_text_plugin(void)
{
    return &text_plugin;
}