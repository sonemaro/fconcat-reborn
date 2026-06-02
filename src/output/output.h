#ifndef OUTPUT_OUTPUT_H
#define OUTPUT_OUTPUT_H

#include "../core/types.h"
#include "../../include/fconcat_api.h"
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct OutputSink OutputSink;

struct OutputSink
{
    void *user_data;
    int (*write)(OutputSink *sink, const char *data, size_t size);
    int (*flush)(OutputSink *sink);
    int (*close)(OutputSink *sink);
    int (*fd)(OutputSink *sink);
};

typedef struct
{
    OutputSink sink;
    FILE *file;
    int owns_file;
} FileOutputSink;

typedef struct
{
    OutputSink sink;
    int fd;
    int owns_fd;
} FdOutputSink;

typedef struct
{
    OutputSink sink;
    OutputSink *downstream;
    char *buffer;
    size_t capacity;
    size_t used;
    int owns_downstream;
    int closed;
} BufferedOutputSink;

int output_sink_write(OutputSink *sink, const char *data, size_t size);
int output_sink_write_cstr(OutputSink *sink, const char *data);
int output_sink_write_fmt(OutputSink *sink, const char *format, ...);
int output_sink_flush(OutputSink *sink);
int output_sink_close(OutputSink *sink);
int output_sink_fd(OutputSink *sink);

void file_output_sink_init(FileOutputSink *sink, FILE *file, int owns_file);
void fd_output_sink_init(FdOutputSink *sink, int fd, int owns_fd);
int buffered_output_sink_init(BufferedOutputSink *sink, OutputSink *downstream,
                              size_t capacity, int owns_downstream);
void buffered_output_sink_destroy(BufferedOutputSink *sink);

int text_begin_document(FconcatContext *ctx);
int text_begin_structure(FconcatContext *ctx);
int text_write_directory(FconcatContext *ctx, const char *path, int level);
int text_write_file_entry(FconcatContext *ctx, const char *path, FileInfo *info);
int text_end_structure(FconcatContext *ctx);
int text_begin_content(FconcatContext *ctx);
int text_write_file_header(FconcatContext *ctx, const char *path);
int text_write_file_chunk(FconcatContext *ctx, const char *data, size_t size);
int text_write_file_footer(FconcatContext *ctx);
int text_write_binary_placeholder(FconcatContext *ctx);
int text_write_symlink_placeholder(FconcatContext *ctx, const char *full_path);
int text_end_content(FconcatContext *ctx);
int text_end_document(FconcatContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* OUTPUT_OUTPUT_H */
