#include "output.h"
#include "../core/context.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OUTPUT_DEFAULT_BUFFER_SIZE (256U * 1024U)
#define OUTPUT_DIRECT_WRITE_THRESHOLD (64U * 1024U)

static int file_sink_write(OutputSink *sink, const char *data, size_t size)
{
    FileOutputSink *file_sink = (FileOutputSink *)sink;
    if (!file_sink || !file_sink->file || !data)
        return -1;

    if (size == 0)
        size = strlen(data);

    return fwrite(data, 1, size, file_sink->file) == size ? 0 : -1;
}

static int file_sink_flush(OutputSink *sink)
{
    FileOutputSink *file_sink = (FileOutputSink *)sink;
    if (!file_sink || !file_sink->file)
        return -1;

    return fflush(file_sink->file) == 0 ? 0 : -1;
}

static int file_sink_close(OutputSink *sink)
{
    FileOutputSink *file_sink = (FileOutputSink *)sink;
    if (!file_sink || !file_sink->file)
        return 0;

    int result = 0;
    if (fflush(file_sink->file) != 0)
        result = -1;

    if (file_sink->owns_file)
    {
        if (fclose(file_sink->file) != 0)
            result = -1;
    }

    file_sink->file = NULL;
    return result;
}

static int file_sink_fd(OutputSink *sink)
{
    FileOutputSink *file_sink = (FileOutputSink *)sink;
    if (!file_sink || !file_sink->file)
        return -1;
    return fileno(file_sink->file);
}

static int fd_sink_write(OutputSink *sink, const char *data, size_t size)
{
    FdOutputSink *fd_sink = (FdOutputSink *)sink;
    if (!fd_sink || fd_sink->fd < 0 || !data)
        return -1;

    if (size == 0)
        size = strlen(data);

    size_t written = 0;
    while (written < size)
    {
        ssize_t n = write(fd_sink->fd, data + written, size - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        written += (size_t)n;
    }

    return 0;
}

static int fd_sink_flush(OutputSink *sink)
{
    (void)sink;
    return 0;
}

static int fd_sink_close(OutputSink *sink)
{
    FdOutputSink *fd_sink = (FdOutputSink *)sink;
    if (!fd_sink || fd_sink->fd < 0)
        return 0;

    int result = 0;
    if (fd_sink->owns_fd && close(fd_sink->fd) != 0)
        result = -1;
    fd_sink->fd = -1;
    return result;
}

static int fd_sink_fd(OutputSink *sink)
{
    FdOutputSink *fd_sink = (FdOutputSink *)sink;
    if (!fd_sink)
        return -1;
    return fd_sink->fd;
}

void file_output_sink_init(FileOutputSink *sink, FILE *file, int owns_file)
{
    if (!sink)
        return;

    memset(sink, 0, sizeof(*sink));
    sink->sink.user_data = sink;
    sink->sink.write = file_sink_write;
    sink->sink.flush = file_sink_flush;
    sink->sink.close = file_sink_close;
    sink->sink.fd = file_sink_fd;
    sink->file = file;
    sink->owns_file = owns_file;
}

void fd_output_sink_init(FdOutputSink *sink, int fd, int owns_fd)
{
    if (!sink)
        return;

    memset(sink, 0, sizeof(*sink));
    sink->sink.user_data = sink;
    sink->sink.write = fd_sink_write;
    sink->sink.flush = fd_sink_flush;
    sink->sink.close = fd_sink_close;
    sink->sink.fd = fd_sink_fd;
    sink->fd = fd;
    sink->owns_fd = owns_fd;
}

static int buffered_sink_drain(BufferedOutputSink *buffered, int flush_downstream)
{
    if (!buffered || !buffered->downstream || buffered->closed)
        return -1;

    if (buffered->used > 0)
    {
        int result = output_sink_write(buffered->downstream, buffered->buffer, buffered->used);
        buffered->used = 0;
        if (result != 0)
            return result;
    }

    return flush_downstream ? output_sink_flush(buffered->downstream) : 0;
}

static int buffered_sink_flush(OutputSink *sink)
{
    return buffered_sink_drain((BufferedOutputSink *)sink, 1);
}

static int buffered_sink_write(OutputSink *sink, const char *data, size_t size)
{
    BufferedOutputSink *buffered = (BufferedOutputSink *)sink;
    if (!buffered || !buffered->downstream || !data || buffered->closed)
        return -1;

    if (size == 0)
        size = strlen(data);
    if (size == 0)
        return 0;

    if (size >= OUTPUT_DIRECT_WRITE_THRESHOLD)
    {
        if (buffered->used > 0 && buffered_sink_drain(buffered, 0) != 0)
            return -1;
        return output_sink_write(buffered->downstream, data, size);
    }

    size_t written = 0;
    while (written < size)
    {
        size_t available = buffered->capacity - buffered->used;
        if (available == 0)
        {
            if (buffered_sink_drain(buffered, 0) != 0)
                return -1;
            available = buffered->capacity;
        }

        size_t to_copy = size - written;
        if (to_copy > available)
            to_copy = available;
        memcpy(buffered->buffer + buffered->used, data + written, to_copy);
        buffered->used += to_copy;
        written += to_copy;
    }

    return 0;
}

static int buffered_sink_close(OutputSink *sink)
{
    BufferedOutputSink *buffered = (BufferedOutputSink *)sink;
    if (!buffered || buffered->closed)
        return 0;

    int result = 0;
    if (buffered->downstream)
    {
        if (buffered_sink_drain(buffered, 0) != 0)
            result = -1;

        if (output_sink_flush(buffered->downstream) != 0)
            result = -1;
        if (buffered->owns_downstream && output_sink_close(buffered->downstream) != 0)
            result = -1;
    }

    buffered->closed = 1;
    return result;
}

static int buffered_sink_fd(OutputSink *sink)
{
    BufferedOutputSink *buffered = (BufferedOutputSink *)sink;
    if (!buffered || !buffered->downstream)
        return -1;
    return output_sink_fd(buffered->downstream);
}

int buffered_output_sink_init(BufferedOutputSink *sink, OutputSink *downstream,
                              size_t capacity, int owns_downstream)
{
    if (!sink || !downstream)
        return -1;

    if (capacity == 0)
        capacity = OUTPUT_DEFAULT_BUFFER_SIZE;

    memset(sink, 0, sizeof(*sink));
    sink->buffer = malloc(capacity);
    if (!sink->buffer)
        return -1;

    sink->sink.user_data = sink;
    sink->sink.write = buffered_sink_write;
    sink->sink.flush = buffered_sink_flush;
    sink->sink.close = buffered_sink_close;
    sink->sink.fd = buffered_sink_fd;
    sink->downstream = downstream;
    sink->capacity = capacity;
    sink->owns_downstream = owns_downstream;
    return 0;
}

void buffered_output_sink_destroy(BufferedOutputSink *sink)
{
    if (!sink)
        return;

    free(sink->buffer);
    memset(sink, 0, sizeof(*sink));
}

int output_sink_write(OutputSink *sink, const char *data, size_t size)
{
    if (!sink || !sink->write || !data)
        return -1;

    return sink->write(sink, data, size);
}

int output_sink_write_cstr(OutputSink *sink, const char *data)
{
    return output_sink_write(sink, data, 0);
}

int output_sink_write_fmt(OutputSink *sink, const char *format, ...)
{
    if (!sink || !format)
        return -1;

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
        return output_sink_write(sink, stack_buf, (size_t)needed);
    }

    char *heap_buf = malloc((size_t)needed + 1);
    if (!heap_buf)
    {
        va_end(args);
        return -1;
    }

    vsnprintf(heap_buf, (size_t)needed + 1, format, args);
    va_end(args);
    int result = output_sink_write(sink, heap_buf, (size_t)needed);
    free(heap_buf);
    return result;
}

int output_sink_flush(OutputSink *sink)
{
    if (!sink)
        return -1;
    return sink->flush ? sink->flush(sink) : 0;
}

int output_sink_close(OutputSink *sink)
{
    if (!sink)
        return 0;
    return sink->close ? sink->close(sink) : 0;
}

int output_sink_fd(OutputSink *sink)
{
    if (!sink || !sink->fd)
        return -1;
    return sink->fd(sink);
}

int text_begin_document(FconcatContext *ctx)
{
    (void)ctx;
    return 0;
}

int text_begin_structure(FconcatContext *ctx)
{
    return ctx->write_output(ctx, "Directory Structure:\n==================\n\n", 0);
}

int text_write_directory(FconcatContext *ctx, const char *path, int level)
{
    if (!ctx || !path)
        return -1;

    size_t indent = (size_t)level * 2;
    size_t path_len = strlen(path);
    char line[MAX_PATH + 640];
    if (indent + 6 + path_len + 1 < sizeof(line))
    {
        memset(line, ' ', indent);
        memcpy(line + indent, "DIR  ", 5);
        memcpy(line + indent + 5, path, path_len);
        memcpy(line + indent + 5 + path_len, "/\n", 2);
        return ctx->write_output(ctx, line, indent + 5 + path_len + 2);
    }

    for (size_t i = 0; i < indent; i++)
    {
        if (ctx->write_output(ctx, " ", 1) != 0)
            return -1;
    }
    if (ctx->write_output(ctx, "DIR  ", 0) != 0 ||
        ctx->write_output(ctx, path, 0) != 0)
        return -1;
    return ctx->write_output(ctx, "/\n", 2);
}

int text_write_file_entry(FconcatContext *ctx, const char *path, FileInfo *info)
{
    if (!ctx || !path)
        return -1;

    size_t indent = (size_t)ctx->current_directory_level * 2;
    char line[MAX_PATH + 640];
    size_t used = 0;
    if (indent + 6 < sizeof(line))
    {
        memset(line, ' ', indent);
        used = indent;
        memcpy(line + used, "FILE ", 5);
        used += 5;
    }
    else
    {
        for (size_t i = 0; i < indent; i++)
        {
            if (ctx->write_output(ctx, " ", 1) != 0)
                return -1;
        }
        if (ctx->write_output(ctx, "FILE ", 0) != 0)
            return -1;
    }

    char size_buf[64];
    size_t size_len = 0;
    if (ctx->get_config_bool(ctx, "show_size") && info)
    {
        size_t kb = (info->size + 1023) / 1024;
        if (kb == 0 && info->size > 0)
            kb = 1;
        int n = snprintf(size_buf, sizeof(size_buf), "[%zu KB] ", kb);
        if (n < 0 || (size_t)n >= sizeof(size_buf))
            return -1;
        size_len = (size_t)n;
    }

    size_t path_len = strlen(path);
    if (used > 0 && used + size_len + path_len + 1 < sizeof(line))
    {
        if (size_len > 0)
        {
            memcpy(line + used, size_buf, size_len);
            used += size_len;
        }
        memcpy(line + used, path, path_len);
        used += path_len;
        line[used++] = '\n';
        return ctx->write_output(ctx, line, used);
    }

    if (size_len > 0 && ctx->write_output(ctx, size_buf, size_len) != 0)
        return -1;
    if (ctx->write_output(ctx, path, 0) != 0)
        return -1;
    return ctx->write_output(ctx, "\n", 1);
}

int text_end_structure(FconcatContext *ctx)
{
    (void)ctx;
    return 0;
}

int text_begin_content(FconcatContext *ctx)
{
    return ctx->write_output(ctx, "\nFile Contents:\n=============\n\n", 0);
}

int text_write_file_header(FconcatContext *ctx, const char *path)
{
    if (!ctx || !path)
        return -1;

    size_t path_len = strlen(path);
    char line[MAX_PATH + 32];
    if (path_len + 10 < sizeof(line))
    {
        memcpy(line, "// File: ", 9);
        memcpy(line + 9, path, path_len);
        line[9 + path_len] = '\n';
        return ctx->write_output(ctx, line, 10 + path_len);
    }

    if (ctx->write_output(ctx, "// File: ", 0) != 0 ||
        ctx->write_output(ctx, path, 0) != 0)
        return -1;
    return ctx->write_output(ctx, "\n", 1);
}

int text_write_file_chunk(FconcatContext *ctx, const char *data, size_t size)
{
    return ctx->write_output(ctx, data, size);
}

int text_write_file_footer(FconcatContext *ctx)
{
    return ctx->write_output(ctx, "\n\n", 2);
}

int text_write_binary_placeholder(FconcatContext *ctx)
{
    return ctx->write_output(ctx, "// [Binary file content not displayed]\n", 0);
}

int text_write_symlink_placeholder(FconcatContext *ctx, const char *full_path)
{
    if (!ctx || !full_path)
        return -1;

    char target[2048];
    ssize_t len = readlink(full_path, target, sizeof(target) - 1);
    if (len > 0)
    {
        target[len] = '\0';
        return ctx->write_output_fmt(ctx, "// [Symbolic link to: %s]\n", target) < 0 ? -1 : 0;
    }

    return ctx->write_output_fmt(ctx, "// [Symbolic link target unreadable: %s]\n", strerror(errno)) < 0 ? -1 : 0;
}

int text_end_content(FconcatContext *ctx)
{
    (void)ctx;
    return 0;
}

int text_end_document(FconcatContext *ctx)
{
    return output_sink_flush(((InternalContextState *)ctx->internal_state)->output_sink);
}
