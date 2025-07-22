#ifndef FCONCAT_CONTENT_H
#define FCONCAT_CONTENT_H

#include "fconcat_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Forward declaration
    struct FileInfo;

    // Plugin capabilities
    typedef enum
    {
        PLUGIN_CAP_CONTENT_TRANSFORM = 1 << 0,
        PLUGIN_CAP_LANGUAGE_AWARE = 1 << 1,
        PLUGIN_CAP_BINARY_SUPPORT = 1 << 2,
        PLUGIN_CAP_STREAMING = 1 << 3
    } PluginCapabilities;

    // Content plugin interface
    typedef struct
    {
        const char *name;
        const char *version;
        const char *description;

        // Lifecycle
        int (*init)(FconcatContext *ctx);
        void (*cleanup)(FconcatContext *ctx);

        // Per-file processing
        PluginFileContext *(*file_start)(FconcatContext *ctx, const char *path, struct FileInfo *info);
        int (*process_chunk)(FconcatContext *ctx, PluginFileContext *file_ctx,
                             const char *input, size_t input_size,
                             char **output, size_t *output_size);
        int (*file_end)(FconcatContext *ctx, PluginFileContext *file_ctx);
        void (*file_cleanup)(FconcatContext *ctx, PluginFileContext *file_ctx);

        // Plugin metadata
        PluginCapabilities capabilities;

    } ContentPlugin;

#ifdef __cplusplus
}
#endif

#endif /* FCONCAT_CONTENT_H */