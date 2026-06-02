#include "filter.h"
#include "../core/types.h"
#include <stdlib.h>
#include <string.h>

typedef struct
{
    BinaryHandling handling;
} BinaryContext;

static int binary_match_path(const char *path, FileInfo *info, void *context)
{
    BinaryContext *ctx = (BinaryContext *)context;
    (void)path;
    if (!ctx || !info)
        return 0;

    return ctx->handling == BINARY_SKIP && info->is_binary;
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
            .match_path = binary_match_path,
            .destroy_context = destroy_binary_context,
            .context = ctx};

        int result = filter_engine_add_rule_internal(engine, &rule);
        if (result != 0)
            free(ctx);
        return result;
    }

    // Placeholder and include modes are handled while streaming file content.
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
