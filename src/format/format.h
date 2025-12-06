#ifndef FORMAT_FORMAT_H
#define FORMAT_FORMAT_H

#include "../core/types.h"
#include "../../include/fconcat_format.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Forward declaration
    struct FconcatContext;

    // Format engine
    typedef struct FormatEngine
    {
        FormatPlugin *plugins[MAX_PLUGINS];
        int plugin_count;
        FormatPlugin *active_formatter;
        FILE *output_file;
        const ResolvedConfig *config;
        pthread_mutex_t mutex;
    } FormatEngine;

    // Format functions
    FormatEngine *format_engine_create(void);
    void format_engine_destroy(FormatEngine *engine);
    int format_engine_configure(FormatEngine *engine, const ResolvedConfig *config, FILE *output_file);
    int format_engine_register_plugin(FormatEngine *engine, FormatPlugin *plugin);
    int format_engine_set_active_formatter(FormatEngine *engine, const char *name);
    int format_engine_begin_document(FormatEngine *engine, struct FconcatContext *ctx);
    int format_engine_begin_structure(FormatEngine *engine, struct FconcatContext *ctx);
    int format_engine_write_directory(FormatEngine *engine, struct FconcatContext *ctx, const char *path, int level);
    int format_engine_write_file_entry(FormatEngine *engine, struct FconcatContext *ctx, const char *path, FileInfo *info);
    int format_engine_end_structure(FormatEngine *engine, struct FconcatContext *ctx);
    int format_engine_begin_content(FormatEngine *engine, struct FconcatContext *ctx);
    int format_engine_write_file_header(FormatEngine *engine, struct FconcatContext *ctx, const char *path);
    int format_engine_write_file_chunk(FormatEngine *engine, struct FconcatContext *ctx, const char *data, size_t size);
    int format_engine_write_file_footer(FormatEngine *engine, struct FconcatContext *ctx);
    int format_engine_end_content(FormatEngine *engine, struct FconcatContext *ctx);
    int format_engine_end_document(FormatEngine *engine, struct FconcatContext *ctx);

    int format_engine_set_active_formatter_unlocked(FormatEngine *engine, const char *name);

    // Built-in formatters
    FormatPlugin *format_text_plugin(void);

#ifdef __cplusplus
}
#endif

#endif /* FORMAT_FORMAT_H */