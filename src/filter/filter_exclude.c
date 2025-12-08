#include "filter.h"
#include "filter_utils.h"
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <stdio.h>

// Check if path matches any exclude pattern
int exclude_match_path(const char *path, FileInfo *info, void *context)
{
    ExcludeContext *ctx = (ExcludeContext *)context;
    if (!ctx || !path)
        return 0;

    const char *basename = filter_get_basename(path);
    if (!basename)
        return 0;

    // Check each pattern against both full path and basename
    for (int i = 0; i < ctx->pattern_count; i++)
    {
        if (!ctx->patterns[i])
            continue;

        // Check against full path
        if (filter_match_pattern(ctx->patterns[i], path))
        {
            return 1; // Match - should exclude
        }

        // Check against basename
        if (filter_match_pattern(ctx->patterns[i], basename))
        {
            return 1; // Match - should exclude
        }

        // Check if pattern matches directory name for directory exclusion
        if (info && info->is_directory)
        {
            char dir_pattern[1024];
            int ret = snprintf(dir_pattern, sizeof(dir_pattern), "%s/*", ctx->patterns[i]);
            if (ret > 0 && ret < (int)sizeof(dir_pattern))
            {
                if (filter_match_pattern(dir_pattern, path))
                {
                    return 1; // Match - should exclude directory
                }
            }
        }
    }

    return 0; // No match
}

// Create exclude context and add patterns
static ExcludeContext *create_exclude_context(const ResolvedConfig *config)
{
    if (!config || config->exclude_count == 0)
        return NULL;

    ExcludeContext *ctx = malloc(sizeof(ExcludeContext));
    if (!ctx)
        return NULL;

    ctx->patterns = malloc(config->exclude_count * sizeof(char *));
    if (!ctx->patterns)
    {
        free(ctx);
        return NULL;
    }

    ctx->pattern_count = config->exclude_count;

    // Copy patterns and normalize them using utility function
    for (int i = 0; i < config->exclude_count; i++)
    {
        if (config->exclude_patterns[i])
        {
            ctx->patterns[i] = filter_normalize_pattern(config->exclude_patterns[i]);
            if (!ctx->patterns[i])
            {
                // Cleanup on failure
                for (int j = 0; j < i; j++)
                {
                    free(ctx->patterns[j]);
                }
                free(ctx->patterns);
                free(ctx);
                return NULL;
            }
        }
        else
        {
            ctx->patterns[i] = strdup(""); // Empty pattern
            if (!ctx->patterns[i])
            {
                // Cleanup on failure
                for (int j = 0; j < i; j++)
                {
                    free(ctx->patterns[j]);
                }
                free(ctx->patterns);
                free(ctx);
                return NULL;
            }
        }
    }

    return ctx;
}

// Cleanup exclude context
static void destroy_exclude_context(ExcludeContext *ctx)
{
    if (!ctx)
        return;

    for (int i = 0; i < ctx->pattern_count; i++)
    {
        free(ctx->patterns[i]);
    }
    free(ctx->patterns);
    free(ctx);
}

void destroy_exclude_context_wrapper(void *context)
{
    destroy_exclude_context((ExcludeContext *)context);
}

int filter_exclude_patterns_init_internal(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    if (config->exclude_count == 0)
        return 0; // No patterns to exclude

    // Create exclude context
    ExcludeContext *ctx = create_exclude_context(config);
    if (!ctx)
        return -1;

    // Create filter rule
    FilterRule rule = {
        .type = FILTER_TYPE_EXCLUDE,
        .priority = 100,
        .match_path = exclude_match_path,
        .match_content = NULL,
        .transform = NULL,
        .destroy_context = destroy_exclude_context_wrapper,
        .context = ctx};

    int result = filter_engine_add_rule_internal(engine, &rule);
    if (result != 0)
    {
        destroy_exclude_context(ctx);
        return result;
    }

    return 0;
}

int filter_exclude_patterns_init(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    pthread_mutex_lock(&engine->mutex);
    int result = filter_exclude_patterns_init_internal(engine, config);
    pthread_mutex_unlock(&engine->mutex);
    return result;
}