#include "context.h"
#include "file_index.h"
#include "version.h"
#include "../output/output.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>

#define INDEX_STREAM_BUFFER_SIZE (64U * 1024U)
#define INDEX_DIRECT_COPY_CHUNK_SIZE (8U * 1024U * 1024U)

static size_t choose_stream_buffer_size(size_t file_size)
{
    if (file_size > 0 && file_size < 4096)
        return file_size;
    if (file_size < 16384)
        return 4096;
    if (file_size < 65536)
        return 16384;
    return 65536;
}

static int direct_copy_enabled(void)
{
    const char *env = getenv("FCONCAT_DIRECT_COPY");
    return env && strcmp(env, "1") == 0;
}

static size_t stat_size_to_size_t(off_t size)
{
    if (size <= 0)
        return 0;
    if ((uintmax_t)size > (uintmax_t)SIZE_MAX)
        return SIZE_MAX;
    return (size_t)size;
}

static int stream_indexed_file_from(FconcatContext *ctx, const FileIndexEntry *entry, size_t offset,
                                    char *buffer, size_t buffer_size, int use_direct_copy)
{
    if (!ctx || !entry || !buffer || buffer_size == 0)
        return -1;

    InternalContextState *internal = (InternalContextState *)ctx->internal_state;
    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    size_t remaining = offset < entry->info.size ? entry->info.size - offset : 0;

    int input_fd = open(entry->full_path, O_RDONLY);
    if (input_fd < 0)
    {
        if (errno == EACCES)
            ctx->warning(ctx, "Permission denied opening file: %s", entry->full_path);
        else if (errno == ENOENT)
            ctx->warning(ctx, "File disappeared during processing: %s", entry->full_path);
        else
            ctx->warning(ctx, "Cannot open file: %s - %s", entry->full_path, strerror(errno));
        if (stats)
            fconcat_size_increment_saturated(&stats->skipped_files);
        return 0;
    }

    if (offset > 0)
    {
        if (lseek(input_fd, (off_t)offset, SEEK_SET) < 0)
        {
            ctx->warning(ctx, "Cannot seek cached prefix in file: %s - %s",
                         entry->full_path, strerror(errno));
            offset = 0;
            remaining = entry->info.size;
            if (lseek(input_fd, 0, SEEK_SET) < 0)
            {
                close(input_fd);
                return -1;
            }
        }
    }

#ifdef __linux__
    int output_fd = use_direct_copy && internal ? output_sink_fd(internal->output_sink) : -1;
    if (output_fd >= 0 && remaining > 0)
    {
        if (output_sink_flush(internal->output_sink) != 0)
        {
            close(input_fd);
            return -1;
        }

        off_t send_offset = (off_t)offset;
        size_t left = remaining;
        int fallback_to_read = 0;

        while (left > 0)
        {
            size_t chunk = left > INDEX_DIRECT_COPY_CHUNK_SIZE ? INDEX_DIRECT_COPY_CHUNK_SIZE : left;
            ssize_t sent = sendfile(output_fd, input_fd, &send_offset, chunk);
            if (sent < 0)
            {
                if (errno == EINTR)
                    continue;
                if (left == remaining &&
                    (errno == EINVAL || errno == ENOSYS || errno == EXDEV || errno == EOPNOTSUPP))
                {
                    fallback_to_read = 1;
                    break;
                }
                if (lseek(input_fd, send_offset, SEEK_SET) < 0)
                {
                    ctx->warning(ctx, "Cannot recover direct copy for file: %s - %s",
                                 entry->full_path, strerror(errno));
                    close(input_fd);
                    return -1;
                }
                fallback_to_read = 1;
                break;
            }
            if (sent == 0)
            {
                close(input_fd);
                return 0;
            }

            left -= (size_t)sent;
            update_context_progress(ctx, (size_t)sent);
        }

        if (!fallback_to_read)
        {
            close(input_fd);
            return 0;
        }

        offset = (size_t)send_offset;
        remaining = offset < entry->info.size ? entry->info.size - offset : 0;
        if (lseek(input_fd, (off_t)offset, SEEK_SET) < 0)
        {
            close(input_fd);
            return -1;
        }
    }
#endif

    size_t read_size = choose_stream_buffer_size(remaining);
    if (read_size > buffer_size)
        read_size = buffer_size;

    int result = 0;
    for (;;)
    {
        ssize_t bytes_read = read(input_fd, buffer, read_size);
        if (bytes_read < 0)
        {
            if (errno == EINTR)
                continue;
            ctx->warning(ctx, "Read error while processing file: %s", entry->full_path);
            break;
        }
        if (bytes_read == 0)
            break;

        if (text_write_file_chunk(ctx, buffer, bytes_read) != 0)
        {
            result = -1;
            break;
        }
        update_context_progress(ctx, (size_t)bytes_read);
    }

    close(input_fd);
    return result;
}

static int emit_index_structure(FconcatContext *ctx, FileIndex *index,
                                int (*should_stop)(void *user_data), void *user_data)
{
    if (!ctx || !index)
        return -1;

    size_t count = file_index_count(index);
    for (size_t i = 0; i < count; i++)
    {
        if (should_stop && should_stop(user_data))
            return -1;

        FileIndexEntry *entry = file_index_entry(index, i);
        if (!entry)
            return -1;

        ctx->current_file_path = entry->relative_path;
        ctx->current_file_info = &entry->info;
        ctx->current_directory_level = entry->level;

        int result = entry->info.is_directory
                         ? text_write_directory(ctx, entry->relative_path, entry->level)
                         : text_write_file_entry(ctx, entry->relative_path, &entry->info);

        ctx->current_file_info = NULL;
        if (result != 0)
            return result;
    }

    return 0;
}

static int emit_index_content(FconcatContext *ctx, const ResolvedConfig *config, FileIndex *index,
                              char *stream_buffer, size_t stream_buffer_size,
                              int use_direct_copy,
                              int (*should_stop)(void *user_data), void *user_data)
{
    if (!ctx || !config || !index || !stream_buffer || stream_buffer_size == 0)
        return -1;

    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    size_t count = file_index_count(index);

    for (size_t i = 0; i < count; i++)
    {
        if (should_stop && should_stop(user_data))
            return -1;

        FileIndexEntry *entry = file_index_entry(index, i);
        if (!entry)
            return -1;
        if (entry->info.is_directory)
            continue;

        ctx->log(ctx, LOG_DEBUG, "Processing file: %s", entry->relative_path);
        ctx->current_file_path = entry->relative_path;
        ctx->current_file_info = &entry->info;
        ctx->current_directory_level = entry->level;
        ctx->current_file_processed_bytes = 0;

        if (stats)
        {
            fconcat_size_increment_saturated(&stats->processed_files);
            fconcat_size_increment_saturated(&stats->total_files);
            fconcat_size_add_saturated(&stats->total_bytes, entry->info.size);
        }

        if (text_write_file_header(ctx, entry->relative_path) != 0)
        {
            ctx->current_file_info = NULL;
            return -1;
        }

        if (entry->info.size > MAX_FILE_SIZE)
        {
            ctx->warning(ctx, "File too large, skipping (limit %lluMB): %s (%zu bytes)",
                         (unsigned long long)(MAX_FILE_SIZE / (1024 * 1024)),
                         entry->relative_path, entry->info.size);
            if (stats)
                fconcat_size_increment_saturated(&stats->skipped_files);
            if (text_write_file_footer(ctx) != 0)
            {
                ctx->current_file_info = NULL;
                return -1;
            }
            ctx->current_file_info = NULL;
            continue;
        }

        if (entry->info.is_symlink && config->symlink_handling == SYMLINK_PLACEHOLDER)
        {
            if (text_write_symlink_placeholder(ctx, entry->full_path) != 0 ||
                text_write_file_footer(ctx) != 0)
            {
                ctx->current_file_info = NULL;
                return -1;
            }
            ctx->current_file_info = NULL;
            continue;
        }

        if (entry->info.is_binary && config->binary_handling == BINARY_PLACEHOLDER)
        {
            if (text_write_binary_placeholder(ctx) != 0)
            {
                ctx->current_file_info = NULL;
                return -1;
            }
            if (stats)
                fconcat_size_increment_saturated(&stats->skipped_files);
            if (text_write_file_footer(ctx) != 0)
            {
                ctx->current_file_info = NULL;
                return -1;
            }
            ctx->current_file_info = NULL;
            continue;
        }

        int result = 0;
        if (entry->prefix_complete && entry->prefix_size > 0)
        {
            result = text_write_file_chunk(ctx, entry->prefix_data, entry->prefix_size);
            if (result == 0)
                update_context_progress(ctx, entry->prefix_size);
        }
        else if (entry->prefix_size > 0)
        {
            if (text_write_file_chunk(ctx, entry->prefix_data, entry->prefix_size) != 0)
                result = -1;
            else
            {
                update_context_progress(ctx, entry->prefix_size);
                result = stream_indexed_file_from(ctx, entry, entry->prefix_size,
                                                  stream_buffer, stream_buffer_size,
                                                  use_direct_copy);
            }
        }
        else
        {
            result = stream_indexed_file_from(ctx, entry, 0, stream_buffer, stream_buffer_size,
                                              use_direct_copy);
        }

        if (result != 0 || text_write_file_footer(ctx) != 0)
        {
            ctx->current_file_info = NULL;
            return -1;
        }

        ctx->current_file_info = NULL;
    }

    return 0;
}

static int output_path_is_null_device(const char *path)
{
    return path && strcmp(path, "/dev/null") == 0;
}

static int process_null_output_index(FconcatContext *ctx, const ResolvedConfig *config, FileIndex *index,
                                     int (*should_stop)(void *user_data), void *user_data)
{
    if (!ctx || !config || !index)
        return -1;

    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    size_t count = file_index_count(index);
    for (size_t i = 0; i < count; i++)
    {
        if (should_stop && should_stop(user_data))
            return -1;

        FileIndexEntry *entry = file_index_entry(index, i);
        if (!entry)
            return -1;
        if (entry->info.is_directory)
            continue;

        if (stats)
        {
            fconcat_size_increment_saturated(&stats->processed_files);
            fconcat_size_increment_saturated(&stats->total_files);
            fconcat_size_add_saturated(&stats->total_bytes, entry->info.size);
        }

        if (entry->info.size > MAX_FILE_SIZE ||
            (entry->info.is_binary && config->binary_handling == BINARY_PLACEHOLDER) ||
            (entry->info.is_symlink && config->symlink_handling == SYMLINK_PLACEHOLDER))
        {
            if (stats)
                fconcat_size_increment_saturated(&stats->skipped_files);
            continue;
        }

        if (stats)
            fconcat_size_add_saturated(&stats->processed_bytes, entry->info.size);
    }

    return text_end_document(ctx);
}

static size_t resolve_prefix_cache_budget(int null_output)
{
    if (null_output)
        return 0;

    const char *env = getenv("FCONCAT_PREFIX_CACHE_MB");
    if (!env || env[0] == '\0')
        return FILE_INDEX_DEFAULT_PREFIX_BUDGET;

    char *end = NULL;
    unsigned long long mb = strtoull(env, &end, 10);
    if (!end || *end != '\0')
        return FILE_INDEX_DEFAULT_PREFIX_BUDGET;
    if (mb > (SIZE_MAX / (1024ULL * 1024ULL)))
        return FILE_INDEX_DEFAULT_PREFIX_BUDGET;
    return (size_t)mb * 1024ULL * 1024ULL;
}

int process_fconcat_document(FconcatContext *ctx, const ResolvedConfig *config,
                             int (*should_stop)(void *user_data), void *user_data)
{
    if (!ctx || !config || !config->input_directory)
        return -1;

    InternalContextState *internal = (InternalContextState *)ctx->internal_state;
    int null_output = output_path_is_null_device(config->output_file);
    FileIndex *index = file_index_create(resolve_prefix_cache_budget(null_output));
    if (!index)
    {
        ctx->error(ctx, "Failed to allocate file index");
        return -1;
    }

    char *stream_buffer = NULL;
    int result = -1;

    if (file_index_build(index, ctx, config, internal ? internal->filter_engine : NULL,
                         should_stop, user_data) != 0)
        goto cleanup;

    if (should_stop && should_stop(user_data))
        goto cleanup;

    if (null_output)
    {
        result = process_null_output_index(ctx, config, index, should_stop, user_data);
        goto cleanup;
    }

    if (text_begin_document(ctx) != 0)
        goto cleanup;

    if (text_begin_structure(ctx) != 0)
        goto cleanup;

    if (emit_index_structure(ctx, index, should_stop, user_data) != 0)
        goto cleanup;

    if (should_stop && should_stop(user_data))
        goto cleanup;

    if (text_end_structure(ctx) != 0)
        goto cleanup;

    if (text_begin_content(ctx) != 0)
        goto cleanup;

    stream_buffer = malloc(INDEX_STREAM_BUFFER_SIZE);
    if (!stream_buffer)
    {
        ctx->error(ctx, "Failed to allocate stream buffer");
        goto cleanup;
    }

    if (emit_index_content(ctx, config, index, stream_buffer, INDEX_STREAM_BUFFER_SIZE,
                           direct_copy_enabled(),
                           should_stop, user_data) != 0)
        goto cleanup;

    if (should_stop && should_stop(user_data))
        goto cleanup;

    if (text_end_content(ctx) != 0)
        goto cleanup;

    result = text_end_document(ctx);

cleanup:
    free(stream_buffer);
    file_index_destroy(index);
    return result;
}

FconcatContext *create_fconcat_context(const ResolvedConfig *config,
                                       OutputSink *output_sink,
                                       ProcessingStats *stats,
                                       ErrorManager *error_manager,
                                       MemoryManager *memory_manager,
                                       struct FilterEngine *filter_engine)
{
    if (!config || !output_sink)
        return NULL;

    // Use heap allocation for context to ensure it's properly isolated
    FconcatContext *ctx = calloc(1, sizeof(FconcatContext));
    if (!ctx)
        return NULL;

    InternalContextState *internal_state = calloc(1, sizeof(InternalContextState));
    if (!internal_state)
    {
        free(ctx);
        return NULL;
    }

    // Initialize internal state
    internal_state->output_file = NULL;
    internal_state->output_sink = output_sink;
    internal_state->config = config;
    internal_state->stats = stats;
    internal_state->error_manager = error_manager;
    internal_state->memory_manager = memory_manager;
    internal_state->filter_engine = filter_engine;
    internal_state->progress_callback = NULL;
    internal_state->progress_user_data = NULL;

    // Initialize context with function pointers
    ctx->config = (const void *)config;
    ctx->get_config_string = context_get_config_string;
    ctx->get_config_int = context_get_config_int;
    ctx->get_config_bool = context_get_config_bool;

    ctx->log = context_log;
    ctx->vlog = context_vlog;
    ctx->is_log_enabled = context_is_log_enabled;

    ctx->alloc = context_alloc;
    ctx->realloc = context_realloc;
    ctx->free = context_free;

    ctx->write_output = context_write_output;
    ctx->write_output_fmt = context_write_output_fmt;

    ctx->error = context_error;
    ctx->warning = context_warning;
    ctx->get_error_count = context_get_error_count;

    ctx->current_file_path = NULL;
    ctx->current_file_info = NULL;
    ctx->current_file_processed_bytes = 0;
    ctx->current_directory_level = 0;

    ctx->stats = (void *)stats;

    ctx->progress = context_progress;
    ctx->set_progress_callback = context_set_progress_callback;

    ctx->create_stream_buffer = context_create_stream_buffer;
    ctx->stream_write = context_stream_write;
    ctx->stream_flush = context_stream_flush;
    ctx->stream_destroy = context_stream_destroy;

    ctx->file_exists = context_file_exists;
    ctx->get_file_info = context_get_file_info;
    ctx->resolve_path = context_resolve_path;

    ctx->fconcat_version = FCONCAT_VERSION;
    ctx->build_info = "Debug build";
    ctx->processing_start_time = time(NULL);
    ctx->output_file_path = config->output_file;

    ctx->internal_state = internal_state;

    return ctx;
}

void destroy_fconcat_context(FconcatContext *ctx)
{
    if (!ctx)
        return;

    free(ctx->internal_state);
    free(ctx);
}

void update_context_for_file(FconcatContext *ctx, const char *filepath, const FileInfo *info)
{
    if (!ctx)
        return;

    ctx->current_file_path = filepath;
    ctx->current_file_info = (const void *)info;
    ctx->current_file_processed_bytes = 0;

    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    if (stats)
    {
        fconcat_size_increment_saturated(&stats->processed_files);
    }
}

void update_context_progress(FconcatContext *ctx, size_t bytes_processed)
{
    if (!ctx)
        return;

    fconcat_size_add_saturated(&ctx->current_file_processed_bytes, bytes_processed);

    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    if (stats)
    {
        fconcat_size_add_saturated(&stats->processed_bytes, bytes_processed);
        stats->current_time = time(NULL);
    }
}

const char *context_get_config_string(FconcatContext *ctx, const char *key)
{
    if (!ctx || !ctx->config || !key)
        return NULL;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;

    if (strcmp(key, "input_directory") == 0)
    {
        return config->input_directory;
    }
    else if (strcmp(key, "output_file") == 0)
    {
        return config->output_file;
    }

    return NULL;
}

int context_get_config_int(FconcatContext *ctx, const char *key)
{
    if (!ctx || !ctx->config || !key)
        return 0;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;

    if (strcmp(key, "binary_handling") == 0)
    {
        return config->binary_handling;
    }
    else if (strcmp(key, "symlink_handling") == 0)
    {
        return config->symlink_handling;
    }
    else if (strcmp(key, "log_level") == 0)
    {
        return config->log_level;
    }

    return 0;
}

bool context_get_config_bool(FconcatContext *ctx, const char *key)
{
    if (!ctx || !ctx->config || !key)
        return false;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;

    if (strcmp(key, "show_size") == 0)
    {
        return config->show_size;
    }
    else if (strcmp(key, "verbose") == 0)
    {
        return config->verbose;
    }
    return false;
}

void context_log(FconcatContext *ctx, LogLevel level, const char *format, ...)
{
    if (!ctx || !format)
        return;

    va_list args;
    va_start(args, format);
    context_vlog(ctx, level, format, args);
    va_end(args);
}

void context_vlog(FconcatContext *ctx, LogLevel level, const char *format, va_list args)
{
    if (!ctx || !format)
        return;

    if (!context_is_log_enabled(ctx, level))
        return;

    const char *level_str;
    switch (level)
    {
    case LOG_ERROR:
        level_str = "ERROR";
        break;
    case LOG_WARNING:
        level_str = "WARNING";
        break;
    case LOG_INFO:
        level_str = "INFO";
        break;
    case LOG_DEBUG:
        level_str = "DEBUG";
        break;
    case LOG_TRACE:
        level_str = "TRACE";
        break;
    default:
        level_str = "UNKNOWN";
        break;
    }

    fprintf(stderr, "[%s] ", level_str);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
}

bool context_is_log_enabled(FconcatContext *ctx, LogLevel level)
{
    if (!ctx)
        return false;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;
    if (config)
    {
        return (int)level <= config->log_level;
    }

    return (int)level <= (int)LOG_INFO;
}

void *context_alloc(FconcatContext *ctx, size_t size)
{
    if (!ctx)
        return malloc(size);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        return memory_alloc(state->memory_manager, size);
    }

    return malloc(size);
}

void *context_realloc(FconcatContext *ctx, void *ptr, size_t size)
{
    if (!ctx)
        return realloc(ptr, size);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        return memory_realloc(state->memory_manager, ptr, size);
    }

    return realloc(ptr, size);
}

void context_free(FconcatContext *ctx, void *ptr)
{
    if (!ctx)
    {
        free(ptr);
        return;
    }

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        memory_free(state->memory_manager, ptr);
    }
    else
    {
        free(ptr);
    }
}

int context_write_output(FconcatContext *ctx, const char *data, size_t size)
{
    if (!ctx || !data)
        return -1;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->output_sink)
    {
        return output_sink_write(state->output_sink, data, size);
    }

    return -1;
}

int context_write_output_fmt(FconcatContext *ctx, const char *format, ...)
{
    if (!ctx || !format)
        return -1;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (!state || !state->output_sink)
    {
        return -1;
    }

    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0)
    {
        va_end(args);
        return -1;
    }

    char stack_buf[512];
    if ((size_t)needed < sizeof(stack_buf))
    {
        vsnprintf(stack_buf, sizeof(stack_buf), format, args);
        va_end(args);
        return output_sink_write(state->output_sink, stack_buf, (size_t)needed);
    }

    char *heap_buf = malloc((size_t)needed + 1);
    if (!heap_buf)
    {
        va_end(args);
        return -1;
    }
    vsnprintf(heap_buf, (size_t)needed + 1, format, args);
    va_end(args);
    int result = output_sink_write(state->output_sink, heap_buf, (size_t)needed);
    free(heap_buf);
    return result;
}

void context_error(FconcatContext *ctx, const char *format, ...)
{
    if (!ctx || !format)
        return;

    va_list args;
    va_start(args, format);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->error_manager)
    {
        context_vlog(ctx, LOG_ERROR, format, args);
    }

    va_end(args);
}

void context_warning(FconcatContext *ctx, const char *format, ...)
{
    if (!ctx || !format)
        return;

    va_list args;
    va_start(args, format);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->error_manager)
    {
        context_vlog(ctx, LOG_WARNING, format, args);
    }

    va_end(args);
}

int context_get_error_count(FconcatContext *ctx)
{
    if (!ctx)
        return 0;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->error_manager)
    {
        return error_get_count(state->error_manager);
    }

    return 0;
}

void context_progress(FconcatContext *ctx, const char *operation, size_t current, size_t total)
{
    if (!ctx)
        return;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->progress_callback)
    {
        state->progress_callback(operation, current, total, state->progress_user_data);
    }
}

void context_set_progress_callback(FconcatContext *ctx, ProgressCallback callback, void *user_data)
{
    if (!ctx)
        return;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state)
    {
        state->progress_callback = callback;
        state->progress_user_data = user_data;
    }
}

void *context_create_stream_buffer(FconcatContext *ctx, size_t initial_size)
{
    if (!ctx)
        return NULL;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        return (void *)stream_buffer_create(state->memory_manager, initial_size);
    }

    return NULL;
}

int context_stream_write(FconcatContext *ctx, void *buffer, const char *data, size_t size)
{
    (void)ctx;
    return stream_buffer_write((StreamBuffer *)buffer, data, size);
}

int context_stream_flush(FconcatContext *ctx, void *buffer)
{
    (void)ctx;
    return stream_buffer_flush((StreamBuffer *)buffer);
}

void context_stream_destroy(FconcatContext *ctx, void *buffer)
{
    (void)ctx;
    stream_buffer_destroy((StreamBuffer *)buffer);
}

bool context_file_exists(FconcatContext *ctx, const char *path)
{
    (void)ctx;
    if (!path)
        return false;

    struct stat st;
    return lstat(path, &st) == 0;  // lstat to not follow symlinks
}

/**
 * @brief Get file information for a path
 * 
 * @param ctx The context (unused but required for API consistency)
 * @param path The file path to query
 * @param info Pointer to FileInfo structure to populate
 * @return 0 on success, -1 on failure
 * 
 * @note IMPORTANT: Caller takes ownership of file_info->path and must free() it.
 *       The path field is allocated with strdup() and the caller is responsible
 *       for freeing it when done with the FileInfo structure.
 */
int context_get_file_info(FconcatContext *ctx, const char *path, void *info)
{
    (void)ctx;
    if (!path || !info)
        return -1;

    FileInfo *file_info = (FileInfo *)info;
    struct stat st;
    // Use lstat to detect symlinks - stat() follows them and can't detect them
    if (lstat(path, &st) != 0)
    {
        return -1;
    }

    file_info->path = strdup(path);
    if (!file_info->path)
        return -1;

    file_info->size = stat_size_to_size_t(st.st_size);
    file_info->modified_time = st.st_mtime;
    file_info->is_directory = S_ISDIR(st.st_mode);
    file_info->is_symlink = S_ISLNK(st.st_mode);
    file_info->is_binary = false; // Would need binary detection
    file_info->permissions = st.st_mode;

    return 0;
}

char *context_resolve_path(FconcatContext *ctx, const char *relative_path)
{
    (void)ctx;
    if (!relative_path)
        return NULL;

    return strdup(relative_path);
}
