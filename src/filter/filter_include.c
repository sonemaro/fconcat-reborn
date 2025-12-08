#include "filter.h"
#include "filter_utils.h"
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <stdio.h>

// Check if path matches any include pattern
int include_match_path(const char *path, FileInfo *info, void *context)
{
    IncludeContext *ctx = (IncludeContext *)context;
    if (!ctx || !path)
        return 0;

    const char *basename = filter_get_basename(path);
    if (!basename)
        return 0;

    // For directories, we should generally include them so we can traverse into them
    // unless they specifically match an exclude pattern later
    if (info && info->is_directory)
    {
        // Check if any pattern might match files within this directory
        for (int i = 0; i < ctx->pattern_count; i++)
        {
            if (!ctx->patterns[i])
                continue;

            // If this is a directory and we have patterns that could match files within it,
            // we should include the directory for traversal
            // This is a heuristic - include directory if it might contain matching files
            return 1;
        }
        return 1; // Include directories by default when using include patterns
    }

    // For files, check each pattern against both full path and basename
    for (int i = 0; i < ctx->pattern_count; i++)
    {
        if (!ctx->patterns[i])
            continue;

        // Check against basename first (most common case)
        if (filter_match_pattern(ctx->patterns[i], basename))
        {
            return 1; // Match - should include
        }

        // Check against full path
        if (filter_match_pattern(ctx->patterns[i], path))
        {
            return 1; // Match - should include
        }

        // For path-based patterns, also try with src/ prefix removed
        if (strncmp(path, "src/", 4) == 0)
        {
            if (filter_match_pattern(ctx->patterns[i], path + 4))
            {
                return 1;
            }
        }
    }

    return 0; // No match
}

// Create include context and add patterns
static IncludeContext *create_include_context(const ResolvedConfig *config)
{
    if (!config || config->include_count == 0)
        return NULL;

    IncludeContext *ctx = malloc(sizeof(IncludeContext));
    if (!ctx)
        return NULL;

    ctx->patterns = malloc(config->include_count * sizeof(char *));
    if (!ctx->patterns)
    {
        free(ctx);
        return NULL;
    }

    ctx->pattern_count = config->include_count;

    // Copy patterns and normalize them using utility function
    for (int i = 0; i < config->include_count; i++)
    {
        if (config->include_patterns[i])
        {
            ctx->patterns[i] = filter_normalize_pattern(config->include_patterns[i]);
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
            if (!ctx->patterns[i]) {
                // Cleanup on failure
                for (int j = 0; j < i; j++) free(ctx->patterns[j]);
                free(ctx->patterns);
                free(ctx);
                return NULL;
            }
        }
    }

    return ctx;
}

// Cleanup include context
static void destroy_include_context(IncludeContext *ctx)
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

void destroy_include_context_wrapper(void *context)
{
    destroy_include_context((IncludeContext *)context);
}

int filter_include_patterns_init_internal(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    if (config->include_count == 0)
    {
        return 0; // No patterns to include
    }

    // Create include context
    IncludeContext *ctx = create_include_context(config);
    if (!ctx)
        return -1;

    // Create filter rule with high priority so it runs first
    FilterRule rule = {
        .type = FILTER_TYPE_INCLUDE,
        .priority = 50,  // Higher priority than exclude patterns
        .match_path = include_match_path,
        .match_content = NULL,
        .transform = NULL,
        .destroy_context = destroy_include_context_wrapper,
        .context = ctx};

    int result = filter_engine_add_rule_internal(engine, &rule);
    if (result != 0)
    {
        destroy_include_context(ctx);
        return result;
    }

    return 0;
}

int filter_include_patterns_init(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    pthread_mutex_lock(&engine->mutex);
    int result = filter_include_patterns_init_internal(engine, config);
    pthread_mutex_unlock(&engine->mutex);
    return result;
}