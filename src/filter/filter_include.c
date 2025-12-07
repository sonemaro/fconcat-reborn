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
static char *str_to_lower_include(const char *str)
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

// Enhanced pattern matching - FIXED for simple patterns like *.tsx
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
    char *lower_pattern = str_to_lower_include(pattern);
    char *lower_string = str_to_lower_include(string);
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
static const char *get_basename_include(const char *path)
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

// Check if path matches any include pattern
int include_match_path(const char *path, FileInfo *info, void *context)
{
    IncludeContext *ctx = (IncludeContext *)context;
    if (!ctx || !path)
        return 0;

    const char *basename = get_basename_include(path);
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
        if (match_pattern_enhanced(ctx->patterns[i], basename))
        {
            return 1; // Match - should include
        }

        // Check against full path
        if (match_pattern_enhanced(ctx->patterns[i], path))
        {
            return 1; // Match - should include
        }

        // For path-based patterns, also try with src/ prefix removed
        if (strncmp(path, "src/", 4) == 0)
        {
            if (match_pattern_enhanced(ctx->patterns[i], path + 4))
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

    // Copy patterns and normalize them
    for (int i = 0; i < config->include_count; i++)
    {
        if (config->include_patterns[i])
        {
            ctx->patterns[i] = strdup(config->include_patterns[i]);
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