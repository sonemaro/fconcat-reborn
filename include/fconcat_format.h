#ifndef FCONCAT_FORMAT_H
#define FCONCAT_FORMAT_H

#include "fconcat_api.h"

#ifdef __cplusplus
extern "C"
{
#endif
    // Format plugin interface
    typedef struct
    {
        const char *name;
        const char *file_extension;
        const char *mime_type;

        // Format lifecycle
        int (*init)(FconcatContext *ctx);
        int (*begin_document)(FconcatContext *ctx);
        int (*begin_structure)(FconcatContext *ctx);
        int (*write_directory)(FconcatContext *ctx, const char *path, int level);
        int (*write_file_entry)(FconcatContext *ctx, const char *path, void *info);
        int (*end_structure)(FconcatContext *ctx);
        int (*begin_content)(FconcatContext *ctx);
        int (*write_file_header)(FconcatContext *ctx, const char *path);
        int (*write_file_chunk)(FconcatContext *ctx, const char *data, size_t size);
        int (*write_file_footer)(FconcatContext *ctx);
        int (*end_content)(FconcatContext *ctx);
        int (*end_document)(FconcatContext *ctx);
        void (*cleanup)(FconcatContext *ctx);

    } FormatPlugin;

#ifdef __cplusplus
}
#endif

#endif /* FCONCAT_FORMAT_H */