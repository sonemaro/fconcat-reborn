#include "filter.h"
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <stdio.h>
#include <ctype.h>

// FNM_CASEFOLD is a GNU extension, not available on macOS
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0
#define NEED_MANUAL_CASEFOLD 1
#endif

// Helper function for case-insensitive string conversion
static char *str_to_lower(const char *str)
{
    if (!str)
        return NULL;
    size_t len = strlen(str);
    char *lower = malloc(len + 1);
    if (!lower)
        return NULL;
    for (size_t i = 0; i <= len; i++) {
        lower[i] = tolower((unsigned char)str[i]);
    }
    return lower;
}

// pattern matching with better wildcard support
static int match_pattern_enhanced(const char *pattern, const char *string)
{
    if (!pattern || !string)
        return 0;

    // Handle empty pattern
    if (strlen(pattern) == 0)
        return 0;

    // For simple patterns like "*.tsx", don't use FNM_PATHNAME
    // FNM_PATHNAME treats '/' specially which breaks simple wildcard matching
    if (fnmatch(pattern, string, 0) == 0)
        return 1;

    // Also try case-insensitive matching
#ifdef NEED_MANUAL_CASEFOLD
    // Manual case-insensitive matching for non-GNU systems (e.g., macOS)
    char *lower_pattern = str_to_lower(pattern);
    char *lower_string = str_to_lower(string);
    int result = 0;
    if (lower_pattern && lower_string) {
        result = (fnmatch(lower_pattern, lower_string, 0) == 0);
    }
    free(lower_pattern);
    free(lower_string);
    if (result)
        return 1;
#else
    if (fnmatch(pattern, string, FNM_CASEFOLD) == 0)
        return 1;
#endif

    return 0;
}

// Get basename from path (handles both / and \ separators)
static const char *get_basename(const char *path)
{
    if (!path)
        return NULL;

    const char *basename = strrchr(path, '/');
    const char *basename_win = strrchr(path, '\\');

    // Use whichever separator was found last
    if (basename_win > basename)
        basename = basename_win;

    return basename ? basename + 1 : path;
}

// Check if path matches any exclude pattern
int exclude_match_path(const char *path, FileInfo *info, void *context)
{
    ExcludeContext *ctx = (ExcludeContext *)context;
    if (!ctx || !path)
        return 0;

    const char *basename = get_basename(path);
    if (!basename)
        return 0;

    // Check each pattern against both full path and basename
    for (int i = 0; i < ctx->pattern_count; i++)
    {
        if (!ctx->patterns[i])
            continue;

        // Check against full path
        if (match_pattern_enhanced(ctx->patterns[i], path))
        {
            return 1; // Match - should exclude
        }

        // Check against basename
        if (match_pattern_enhanced(ctx->patterns[i], basename))
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
                if (match_pattern_enhanced(dir_pattern, path))
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

    // Copy patterns and normalize them
    for (int i = 0; i < config->exclude_count; i++)
    {
        if (config->exclude_patterns[i])
        {
            ctx->patterns[i] = strdup(config->exclude_patterns[i]);
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

            // Normalize pattern (remove leading/trailing whitespace)
            char *pattern = ctx->patterns[i];
            size_t len = strlen(pattern);

            // Remove trailing whitespace
            while (len > 0 && (pattern[len - 1] == ' ' || pattern[len - 1] == '\t'))
            {
                pattern[len - 1] = '\0';
                len--;
            }

            // Skip leading whitespace
            while (*pattern == ' ' || *pattern == '\t')
            {
                pattern++;
            }

            // Update pattern if it changed
            if (pattern != ctx->patterns[i])
            {
                char *new_pattern = strdup(pattern);
                if (new_pattern)
                {
                    free(ctx->patterns[i]);
                    ctx->patterns[i] = new_pattern;
                }
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