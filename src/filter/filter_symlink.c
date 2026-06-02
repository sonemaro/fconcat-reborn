#include "filter.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// Symlink handling context
typedef struct
{
    SymlinkHandling handling;
} SymlinkContext;

static int symlink_match_path(const char *path, FileInfo *info, void *context)
{
    (void)path;
    SymlinkContext *ctx = (SymlinkContext *)context;
    if (!ctx || !info)
        return 0;

    return info->is_symlink;
}

static void destroy_symlink_context(void *context)
{
    free(context);
}

int filter_symlink_handling_init_internal(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    SymlinkContext *ctx = malloc(sizeof(SymlinkContext));
    if (!ctx)
    {
        return -1;
    }

    ctx->handling = config->symlink_handling;

    if (config->symlink_handling == SYMLINK_SKIP)
    {
        // Create exclude rule for symlinks
        FilterRule rule = {
            .type = FILTER_TYPE_EXCLUDE,
            .priority = 80,
            .match_path = symlink_match_path,
            .destroy_context = destroy_symlink_context,
            .context = ctx};

        int result = filter_engine_add_rule_internal(engine, &rule);
        if (result != 0)
            free(ctx);
        return result;
    }
    else if (config->symlink_handling == SYMLINK_FOLLOW)
    {
        // Symlink following is handled in directory traversal
        free(ctx);
        return 0;
    }
    else if (config->symlink_handling == SYMLINK_INCLUDE ||
             config->symlink_handling == SYMLINK_PLACEHOLDER)
    {
        // Include symlinks as-is or write placeholders during content streaming.
        free(ctx);
        return 0;
    }

    free(ctx);
    return 0;
}

int filter_symlink_handling_init(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    pthread_mutex_lock(&engine->mutex);
    int result = filter_symlink_handling_init_internal(engine, config);
    pthread_mutex_unlock(&engine->mutex);
    return result;
}
