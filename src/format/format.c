#include "format.h"
#include "../core/error.h"
#include "../core/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

FormatEngine *format_engine_create(void)
{
    FormatEngine *engine = calloc(1, sizeof(FormatEngine));
    if (!engine)
    {
        return NULL;
    }

    if (pthread_mutex_init(&engine->mutex, NULL) != 0)
    {
        free(engine);
        return NULL;
    }

    // Register built-in formatters
    format_engine_register_plugin(engine, format_text_plugin());

    return engine;
}

void format_engine_destroy(FormatEngine *engine)
{
    if (!engine)
        return;

    pthread_mutex_lock(&engine->mutex);

    // Cleanup plugins
    for (int i = 0; i < engine->plugin_count; i++)
    {
        if (engine->plugins[i] && engine->plugins[i]->cleanup)
        {
            // Note: We need a context here, but for cleanup we'll pass NULL
            engine->plugins[i]->cleanup(NULL);
        }
    }

    pthread_mutex_unlock(&engine->mutex);
    pthread_mutex_destroy(&engine->mutex);
    free(engine);
}

// FIXED: Added internal unlocked version to prevent deadlocks
int format_engine_set_active_formatter_unlocked(FormatEngine *engine, const char *name)
{
    if (!engine || !name)
        return -1;

    for (int i = 0; i < engine->plugin_count; i++)
    {
        if (engine->plugins[i] && engine->plugins[i]->name &&
            strcmp(engine->plugins[i]->name, name) == 0)
        {
            engine->active_formatter = engine->plugins[i];
            return 0;
        }
    }

    return -1;
}

int format_engine_configure(FormatEngine *engine, const ResolvedConfig *config, FILE *output_file)
{
    if (!engine || !config)
        return -1;

    pthread_mutex_lock(&engine->mutex);

    engine->config = config;
    engine->output_file = output_file;

    const char *format_name = config->output_format ? config->output_format : "text";

    if (format_engine_set_active_formatter_unlocked(engine, format_name) != 0)
    {
        // Set default formatter if requested one not found
        if (engine->plugin_count > 0)
        {
            engine->active_formatter = engine->plugins[0];
        }
    }

    pthread_mutex_unlock(&engine->mutex);
    return 0;
}

int format_engine_register_plugin(FormatEngine *engine, FormatPlugin *plugin)
{
    if (!engine || !plugin || engine->plugin_count >= MAX_PLUGINS)
        return -1;

    pthread_mutex_lock(&engine->mutex);

    engine->plugins[engine->plugin_count] = plugin;
    engine->plugin_count++;

    // Set as active if it's the first one
    if (engine->plugin_count == 1)
    {
        engine->active_formatter = plugin;
    }

    pthread_mutex_unlock(&engine->mutex);
    return 0;
}

int format_engine_set_active_formatter(FormatEngine *engine, const char *name)
{
    if (!engine || !name)
        return -1;

    pthread_mutex_lock(&engine->mutex);
    int result = format_engine_set_active_formatter_unlocked(engine, name);
    pthread_mutex_unlock(&engine->mutex);
    return result;
}

int format_engine_begin_document(FormatEngine *engine, FconcatContext *ctx)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->begin_document)
    {
        return engine->active_formatter->begin_document(ctx);
    }

    return 0;
}

int format_engine_begin_structure(FormatEngine *engine, FconcatContext *ctx)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->begin_structure)
    {
        return engine->active_formatter->begin_structure(ctx);
    }

    return 0;
}

int format_engine_write_directory(FormatEngine *engine, FconcatContext *ctx, const char *path, int level)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->write_directory)
    {
        return engine->active_formatter->write_directory(ctx, path, level);
    }

    return 0;
}

int format_engine_write_file_entry(FormatEngine *engine, FconcatContext *ctx, const char *path, FileInfo *info)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->write_file_entry)
    {
        return engine->active_formatter->write_file_entry(ctx, path, info);
    }

    return 0;
}

int format_engine_end_structure(FormatEngine *engine, FconcatContext *ctx)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->end_structure)
    {
        return engine->active_formatter->end_structure(ctx);
    }

    return 0;
}

int format_engine_begin_content(FormatEngine *engine, FconcatContext *ctx)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->begin_content)
    {
        return engine->active_formatter->begin_content(ctx);
    }

    return 0;
}

int format_engine_write_file_header(FormatEngine *engine, FconcatContext *ctx, const char *path)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->write_file_header)
    {
        return engine->active_formatter->write_file_header(ctx, path);
    }

    return 0;
}

int format_engine_write_file_chunk(FormatEngine *engine, FconcatContext *ctx, const char *data, size_t size)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->write_file_chunk)
    {
        return engine->active_formatter->write_file_chunk(ctx, data, size);
    }

    return 0;
}

int format_engine_write_file_footer(FormatEngine *engine, FconcatContext *ctx)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->write_file_footer)
    {
        return engine->active_formatter->write_file_footer(ctx);
    }

    return 0;
}

int format_engine_end_content(FormatEngine *engine, FconcatContext *ctx)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->end_content)
    {
        return engine->active_formatter->end_content(ctx);
    }

    return 0;
}

int format_engine_end_document(FormatEngine *engine, FconcatContext *ctx)
{
    if (!engine || !engine->active_formatter)
        return -1;

    if (engine->active_formatter->end_document)
    {
        return engine->active_formatter->end_document(ctx);
    }

    return 0;
}