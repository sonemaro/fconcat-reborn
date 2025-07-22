#ifndef FCONCAT_FILTER_H
#define FCONCAT_FILTER_H

#include "fconcat_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Filter types
    typedef enum
    {
        FILTER_TYPE_INCLUDE,
        FILTER_TYPE_EXCLUDE,
        FILTER_TYPE_TRANSFORM
    } FilterType;

    // Filter plugin interface
    typedef struct
    {
        const char *name;
        FilterType type;
        int priority;

        // Filter lifecycle
        int (*init)(FconcatContext *ctx);

        // Path-based filtering
        int (*should_include_path)(FconcatContext *ctx, const char *path, void *info);

        // Content-based filtering
        int (*should_include_content)(FconcatContext *ctx, const char *path,
                                      const char *content_sample, size_t sample_size);

        // Content transformation
        int (*transform_content)(FconcatContext *ctx, const char *path,
                                 const char *input, size_t input_size,
                                 char **output, size_t *output_size);

        void (*cleanup)(FconcatContext *ctx);

    } FilterPlugin;

#ifdef __cplusplus
}
#endif

#endif /* FCONCAT_FILTER_H */