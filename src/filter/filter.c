#include "filter.h"
#include "../core/error.h"
#include "../core/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>

#ifndef PATH_SEP
#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif
#endif

static int filter_engine_rule_storage_is_safe(const FilterEngine *engine)
{
    if (!engine)
        return 0;
    if (engine->rule_capacity < 0)
        return 0;
    if (engine->rule_capacity > 0 && !engine->rules)
        return 0;
    if ((size_t)engine->rule_capacity > SIZE_MAX / sizeof(FilterRule))
        return 0;
    return 1;
}

static int filter_engine_rules_are_valid(const FilterEngine *engine)
{
    if (!filter_engine_rule_storage_is_safe(engine))
        return 0;
    return engine->rule_count >= 0 && engine->rule_count <= engine->rule_capacity;
}

static int filter_engine_cleanup_count(const FilterEngine *engine)
{
    if (!filter_engine_rule_storage_is_safe(engine))
        return 0;
    if (engine->rule_count < 0 || engine->rule_count > engine->rule_capacity)
        return engine->rule_capacity;
    return engine->rule_count;
}

char *get_absolute_path_util(const char *path)
{
    if (!path)
        return NULL;

#ifdef _WIN32
    char *abs_path = _fullpath(NULL, path, 0);
#else
    char *abs_path = realpath(path, NULL);
#endif

    if (!abs_path)
    {
        // Fallback to duplicating path
        abs_path = strdup(path);
    }
    return abs_path;
}

const char *get_filename_util(const char *path)
{
    if (!path)
        return NULL;

    const char *basename = strrchr(path, PATH_SEP);
    return basename ? basename + 1 : path;
}

char *get_relative_path_util(const char *base_dir, const char *target_path)
{
    if (!base_dir || !target_path)
        return NULL;

    char *abs_base = get_absolute_path_util(base_dir);
    char *abs_target = get_absolute_path_util(target_path);

    if (!abs_base || !abs_target)
    {
        free(abs_base);
        free(abs_target);
        return NULL;
    }

    size_t base_len = strlen(abs_base);

    // Ensure base path ends with separator for comparison
    if (base_len > 0 && abs_base[base_len - 1] != PATH_SEP)
    {
        char *temp = malloc(base_len + 2);
        if (!temp)
        {
            free(abs_base);
            free(abs_target);
            return NULL;
        }

        strcpy(temp, abs_base);
        temp[base_len] = PATH_SEP;
        temp[base_len + 1] = '\0';
        free(abs_base);
        abs_base = temp;
        base_len++;
    }

    char *result = NULL;
    if (strncmp(abs_target, abs_base, base_len) == 0)
    {
        result = strdup(abs_target + base_len);
    }

    free(abs_base);
    free(abs_target);
    return result;
}

// Check if a file is binary by reading the first portion and looking for null bytes
// Returns: 1 if binary, 0 if text, -1 on error
int filter_is_binary_file(const char *filepath)
{
    if (!filepath)
        return -1;

    FILE *file = fopen(filepath, "rb");
    if (!file)
        return -1;

    unsigned char buffer[BINARY_CHECK_SIZE];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);

    if (bytes_read == 0)
        return 0; // Empty file is treated as text

    // Check for null bytes - primary binary indicator
    for (size_t i = 0; i < bytes_read; i++)
    {
        if (buffer[i] == 0)
            return 1; // Binary
    }

    return 0; // Text
}

FilterEngine *filter_engine_create(void)
{
    FilterEngine *engine = calloc(1, sizeof(FilterEngine));
    if (!engine)
    {
        return NULL;
    }

    if (pthread_mutex_init(&engine->mutex, NULL) != 0)
    {
        free(engine);
        return NULL;
    }

    engine->rule_capacity = 100;
    engine->rules = calloc(engine->rule_capacity, sizeof(FilterRule));
    if (!engine->rules)
    {
        pthread_mutex_destroy(&engine->mutex);
        free(engine);
        return NULL;
    }

    return engine;
}

void filter_engine_destroy(FilterEngine *engine)
{
    if (!engine)
        return;

    pthread_mutex_lock(&engine->mutex);

    // Cleanup rule contexts
    int rule_count = filter_engine_cleanup_count(engine);
    for (int i = 0; i < rule_count; i++)
    {
        FilterRule *rule = &engine->rules[i];
        if (rule->context && rule->destroy_context)
        {
            rule->destroy_context(rule->context);
        }
    }

    // Cleanup rules
    free(engine->rules);

    pthread_mutex_unlock(&engine->mutex);
    pthread_mutex_destroy(&engine->mutex);
    free(engine);
}

static int add_output_file_exclusion(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config || !config->output_file || !config->input_directory)
    {
        return 0;
    }

    // Get absolute paths
    char *abs_input = get_absolute_path_util(config->input_directory);
    char *abs_output = get_absolute_path_util(config->output_file);

    if (!abs_input || !abs_output)
    {
        free(abs_input);
        free(abs_output);
        return -1;
    }

    // Check if output is inside input
    size_t input_len = strlen(abs_input);

    // Normalize paths for comparison
    char *normalized_input = abs_input;
    if (input_len > 0 && abs_input[input_len - 1] != PATH_SEP)
    {
        normalized_input = malloc(input_len + 2);
        if (!normalized_input)
        {
            free(abs_input);
            free(abs_output);
            return -1;
        }

        strcpy(normalized_input, abs_input);
        normalized_input[input_len] = PATH_SEP;
        normalized_input[input_len + 1] = '\0';
        input_len++;
    }

    bool output_inside_input = (strncmp(abs_output, normalized_input, input_len) == 0);

    if (output_inside_input)
    {
        // Create exclusion context for output file
        ExcludeContext *ctx = malloc(sizeof(ExcludeContext));
        if (!ctx)
        {
            if (normalized_input != abs_input)
                free(normalized_input);
            free(abs_input);
            free(abs_output);
            return -1;
        }

        // Create patterns array (max 3 patterns)
        ctx->patterns = malloc(3 * sizeof(char *));
        if (!ctx->patterns)
        {
            free(ctx);
            if (normalized_input != abs_input)
                free(normalized_input);
            free(abs_input);
            free(abs_output);
            return -1;
        }

        ctx->pattern_count = 0;

        ctx->patterns[ctx->pattern_count] = strdup(abs_output);
        if (!ctx->patterns[ctx->pattern_count])
        {
            destroy_exclude_context_wrapper(ctx);
            if (normalized_input != abs_input)
                free(normalized_input);
            free(abs_input);
            free(abs_output);
            return -1;
        }
        ctx->pattern_count++;

        // Add relative path
        char *rel_path = get_relative_path_util(config->input_directory, config->output_file);
        if (rel_path)
        {
            ctx->patterns[ctx->pattern_count++] = rel_path;
        }

        // Add basename
        ctx->patterns[ctx->pattern_count] = strdup(get_filename_util(config->output_file));
        if (!ctx->patterns[ctx->pattern_count])
        {
            destroy_exclude_context_wrapper(ctx);
            if (normalized_input != abs_input)
                free(normalized_input);
            free(abs_input);
            free(abs_output);
            return -1;
        }
        ctx->pattern_count++;

        // Create filter rule
        FilterRule rule = {
            .type = FILTER_TYPE_EXCLUDE,
            .priority = 200, // Higher priority than user patterns
            .match_path = exclude_match_path,
            .destroy_context = destroy_exclude_context_wrapper,
            .context = ctx};

        if (filter_engine_add_rule_internal(engine, &rule) != 0)
        {
            // Clean up ctx on failure - it wasn't added to the engine
            destroy_exclude_context_wrapper(ctx);
            if (normalized_input != abs_input)
                free(normalized_input);
            free(abs_input);
            free(abs_output);
            return -1;
        }
    }

    if (normalized_input != abs_input)
        free(normalized_input);
    free(abs_input);
    free(abs_output);
    return 0;
}

int filter_engine_configure(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    pthread_mutex_lock(&engine->mutex);

    engine->config = config;
    int result = 0;

    // SUPER IMPORTANT: Prevents endless loop if src and dst are the same
    if (add_output_file_exclusion(engine, config) != 0)
        result = -1;

    // Initialize built-in filters
    if (result == 0 && filter_include_patterns_init_internal(engine, config) != 0)
        result = -1;
    if (result == 0 && filter_exclude_patterns_init_internal(engine, config) != 0)
        result = -1;
    if (result == 0 && filter_binary_detection_init_internal(engine, config) != 0)
        result = -1;
    if (result == 0 && filter_symlink_handling_init_internal(engine, config) != 0)
        result = -1;

    pthread_mutex_unlock(&engine->mutex);

    return result;
}

int filter_engine_add_rule_internal(FilterEngine *engine, const FilterRule *rule)
{
    if (!engine || !rule)
        return -1;
    if (!filter_engine_rules_are_valid(engine) || engine->rule_capacity == 0)
        return -1;

    if (engine->rule_count >= engine->rule_capacity)
    {
        // Resize rules array
        if (engine->rule_capacity > INT_MAX / 2)
            return -1;
        int new_capacity = engine->rule_capacity * 2;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(FilterRule))
            return -1;
        FilterRule *new_rules = realloc(engine->rules, new_capacity * sizeof(FilterRule));
        if (!new_rules)
        {
            return -1;
        }
        memset(new_rules + engine->rule_capacity, 0,
               (size_t)(new_capacity - engine->rule_capacity) * sizeof(FilterRule));
        engine->rules = new_rules;
        engine->rule_capacity = new_capacity;
    }

    engine->rules[engine->rule_count] = *rule;
    engine->rule_count++;

    return 0;
}

int filter_engine_add_rule(FilterEngine *engine, FilterRule *rule)
{
    if (!engine || !rule)
        return -1;

    pthread_mutex_lock(&engine->mutex);

    int result = filter_engine_add_rule_internal(engine, rule);

    pthread_mutex_unlock(&engine->mutex);

    return result;
}

int filter_engine_should_include_path(FilterEngine *engine, FconcatContext *ctx, const char *path, FileInfo *info)
{
    (void)ctx;
    if (!engine || !path)
        return 1;
    if (!filter_engine_rules_are_valid(engine))
        return 1;

    // Check include rules first - if any include patterns are specified,
    // the file must match at least one include pattern
    bool has_include_rules = false;
    bool matches_include = false;

    for (int i = 0; i < engine->rule_count; i++)
    {
        FilterRule *rule = &engine->rules[i];

        if (rule->type == FILTER_TYPE_INCLUDE && rule->match_path)
        {
            has_include_rules = true;
            int result = rule->match_path(path, info, rule->context);
            if (result)
            {
                matches_include = true;
                break;  // Found a matching include pattern
            }
        }
    }

    // If there are include rules but this path doesn't match any, exclude it
    if (has_include_rules && !matches_include)
    {
        return 0;
    }

    // Check exclude rules
    for (int i = 0; i < engine->rule_count; i++)
    {
        FilterRule *rule = &engine->rules[i];

        if (rule->type == FILTER_TYPE_EXCLUDE && rule->match_path)
        {
            int result = rule->match_path(path, info, rule->context);
            if (result)
            {
                return 0; // Exclude this path
            }
        }
    }

    return 1; // Include by default
}
