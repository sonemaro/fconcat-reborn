#include "filter.h"
#include <stdlib.h>
#include <string.h>

// Binary detection context
typedef struct
{
    BinaryHandling handling;
} BinaryContext;

static int binary_match_content(const char *path, const char *content, size_t size, void *context)
{
    BinaryContext *ctx = (BinaryContext *)context;
    if (!ctx || !content)
        return 0;

    (void)path; // Mark as intentionally unused

    // Simple binary detection - look for null bytes
    for (size_t i = 0; i < size && i < 1024; i++)
    {
        if (content[i] == '\0')
        {
            return 1; // Binary detected
        }
    }

    return 0; // Not binary
}

static int binary_transform(const char *path, const char *input, size_t input_size, char **output, size_t *output_size, void *context)
{
    BinaryContext *ctx = (BinaryContext *)context;
    if (!ctx || !input || !output || !output_size)
        return -1;

    (void)path;       // Unused
    (void)input;      // Unused
    (void)input_size; // Unused

    if (ctx->handling == BINARY_PLACEHOLDER)
    {
        const char *placeholder = "// [Binary file content not displayed]\n";
        size_t placeholder_len = strlen(placeholder);

        *output = malloc(placeholder_len);
        if (!*output)
            return -1;

        memcpy(*output, placeholder, placeholder_len);
        *output_size = placeholder_len;

        return 0;
    }

    // For BINARY_INCLUDE or BINARY_SKIP, don't transform
    return -1;
}

static void destroy_binary_context(void *context)
{
    free(context);
}

int filter_binary_detection_init_internal(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    BinaryContext *ctx = malloc(sizeof(BinaryContext));
    if (!ctx)
    {
        return -1;
    }

    ctx->handling = config->binary_handling;

    if (config->binary_handling == BINARY_SKIP)
    {
        // Create exclude rule for binary files
        FilterRule rule = {
            .type = FILTER_TYPE_EXCLUDE,
            .priority = 90,
            .match_path = NULL,
            .match_content = binary_match_content,
            .transform = NULL,
            .destroy_context = destroy_binary_context,
            .context = ctx};

        int result = filter_engine_add_rule_internal(engine, &rule);
        return result;
    }
    else if (config->binary_handling == BINARY_PLACEHOLDER)
    {
        // Create transform rule for binary files
        FilterRule rule = {
            .type = FILTER_TYPE_TRANSFORM,
            .priority = 90,
            .match_path = NULL,
            .match_content = binary_match_content,
            .transform = binary_transform,
            .destroy_context = destroy_binary_context,
            .context = ctx};

        int result = filter_engine_add_rule_internal(engine, &rule);
        return result;
    }

    // For BINARY_INCLUDE, don't add any rules
    free(ctx);
    return 0;
}

int filter_binary_detection_init(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    pthread_mutex_lock(&engine->mutex);
    int result = filter_binary_detection_init_internal(engine, config);
    pthread_mutex_unlock(&engine->mutex);
    return result;
}