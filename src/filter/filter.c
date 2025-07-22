#include "filter.h"
#include "../core/error.h"
#include "../core/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fnmatch.h>
#include <limits.h>

#ifndef PATH_SEP
#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif
#endif

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
        if (temp)
        {
            strcpy(temp, abs_base);
            temp[base_len] = PATH_SEP;
            temp[base_len + 1] = '\0';
            free(abs_base);
            abs_base = temp;
            base_len++;
        }
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

    // Cleanup plugins
    for (int i = 0; i < engine->plugin_count; i++)
    {
        if (engine->plugins[i] && engine->plugins[i]->cleanup)
        {
            engine->plugins[i]->cleanup(NULL);
        }
    }

    // Cleanup rule contexts
    for (int i = 0; i < engine->rule_count; i++)
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
        if (normalized_input)
        {
            strcpy(normalized_input, abs_input);
            normalized_input[input_len] = PATH_SEP;
            normalized_input[input_len + 1] = '\0';
            input_len++;
        }
        else
        {
            normalized_input = abs_input;
        }
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

        // Add absolute path
        ctx->patterns[ctx->pattern_count++] = strdup(abs_output);

        // Add relative path
        char *rel_path = get_relative_path_util(config->input_directory, config->output_file);
        if (rel_path)
        {
            ctx->patterns[ctx->pattern_count++] = rel_path;
        }

        // Add basename
        ctx->patterns[ctx->pattern_count++] = strdup(get_filename_util(config->output_file));

        // Create filter rule
        FilterRule rule = {
            .type = FILTER_TYPE_EXCLUDE,
            .priority = 200, // Higher priority than user patterns
            .match_path = exclude_match_path,
            .match_content = NULL,
            .transform = NULL,
            .destroy_context = destroy_exclude_context_wrapper,
            .context = ctx};

        filter_engine_add_rule_internal(engine, &rule);
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

    // SUPER IMPORTANT: Prevents endless loop if src and dst are the same
    add_output_file_exclusion(engine, config);

    // Initialize built-in filters
    filter_exclude_patterns_init_internal(engine, config);
    filter_binary_detection_init_internal(engine, config);
    filter_symlink_handling_init_internal(engine, config);

    pthread_mutex_unlock(&engine->mutex);

    return 0;
}

int filter_engine_register_plugin(FilterEngine *engine, FilterPlugin *plugin)
{
    if (!engine || !plugin || engine->plugin_count >= MAX_PLUGINS)
        return -1;

    pthread_mutex_lock(&engine->mutex);

    engine->plugins[engine->plugin_count] = plugin;
    engine->plugin_count++;

    pthread_mutex_unlock(&engine->mutex);
    return 0;
}

int filter_engine_add_rule_internal(FilterEngine *engine, FilterRule *rule)
{
    if (!engine || !rule)
        return -1;

    if (engine->rule_count >= engine->rule_capacity)
    {
        // Resize rules array
        int new_capacity = engine->rule_capacity * 2;
        FilterRule *new_rules = realloc(engine->rules, new_capacity * sizeof(FilterRule));
        if (!new_rules)
        {
            return -1;
        }
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
    if (!engine || !path)
        return 1;

    pthread_mutex_lock(&engine->mutex);

    // Check rules in priority order
    for (int i = 0; i < engine->rule_count; i++)
    {
        FilterRule *rule = &engine->rules[i];

        if (rule->match_path)
        {
            int result = rule->match_path(path, info, rule->context);

            if (rule->type == FILTER_TYPE_EXCLUDE && result)
            {
                pthread_mutex_unlock(&engine->mutex);
                return 0; // Exclude this path
            }
            else if (rule->type == FILTER_TYPE_INCLUDE && !result)
            {
                pthread_mutex_unlock(&engine->mutex);
                return 0; // Don't include this path
            }
        }
    }

    // Check plugins
    for (int i = 0; i < engine->plugin_count; i++)
    {
        FilterPlugin *plugin = engine->plugins[i];
        if (plugin && plugin->should_include_path)
        {
            int result = plugin->should_include_path(ctx, path, info);
            if (!result)
            {
                pthread_mutex_unlock(&engine->mutex);
                return 0; // Plugin excluded this path
            }
        }
    }

    pthread_mutex_unlock(&engine->mutex);
    return 1; // Include by default
}

int filter_engine_should_include_content(FilterEngine *engine, FconcatContext *ctx, const char *path, const char *content, size_t size)
{
    if (!engine || !path || !content)
        return 1;

    pthread_mutex_lock(&engine->mutex);

    // Check rules
    for (int i = 0; i < engine->rule_count; i++)
    {
        FilterRule *rule = &engine->rules[i];

        if (rule->match_content)
        {
            int result = rule->match_content(path, content, size, rule->context);

            if (rule->type == FILTER_TYPE_EXCLUDE && result)
            {
                pthread_mutex_unlock(&engine->mutex);
                return 0; // Exclude this content
            }
            else if (rule->type == FILTER_TYPE_INCLUDE && !result)
            {
                pthread_mutex_unlock(&engine->mutex);
                return 0; // Don't include this content
            }
        }
    }

    // Check plugins
    for (int i = 0; i < engine->plugin_count; i++)
    {
        FilterPlugin *plugin = engine->plugins[i];
        if (plugin && plugin->should_include_content)
        {
            int result = plugin->should_include_content(ctx, path, content, size);
            if (!result)
            {
                pthread_mutex_unlock(&engine->mutex);
                return 0; // Plugin excluded this content
            }
        }
    }

    pthread_mutex_unlock(&engine->mutex);
    return 1; // Include by default
}

int filter_engine_transform_content(FilterEngine *engine, FconcatContext *ctx, const char *path, const char *input, size_t input_size, char **output, size_t *output_size)
{
    if (!engine || !path || !input || !output || !output_size)
        return -1;

    pthread_mutex_lock(&engine->mutex);

    // Get internal state to access memory manager
    InternalContextState *internal = (InternalContextState *)ctx->internal_state;

    // Start with input data - use buffer pool
    char *current_data = memory_get_buffer(internal->memory_manager, input_size);
    if (!current_data)
    {
        pthread_mutex_unlock(&engine->mutex);
        return -1;
    }
    memcpy(current_data, input, input_size);
    size_t current_size = input_size;

    // Apply transform rules
    for (int i = 0; i < engine->rule_count; i++)
    {
        FilterRule *rule = &engine->rules[i];

        if (rule->type == FILTER_TYPE_TRANSFORM && rule->transform)
        {
            char *transformed_data = NULL;
            size_t transformed_size = 0;

            int result = rule->transform(path, current_data, current_size, &transformed_data, &transformed_size, rule->context);

            if (result == 0 && transformed_data)
            {
                // Release old buffer back to pool
                memory_release_buffer(internal->memory_manager, current_data);

                // Use transformed data (might be from malloc or pool)
                current_data = transformed_data;
                current_size = transformed_size;
            }
        }
    }

    // Apply plugin transformations
    for (int i = 0; i < engine->plugin_count; i++)
    {
        FilterPlugin *plugin = engine->plugins[i];
        if (plugin && plugin->transform_content)
        {
            char *transformed_data = NULL;
            size_t transformed_size = 0;

            int result = plugin->transform_content(ctx, path, current_data, current_size, &transformed_data, &transformed_size);

            if (result == 0 && transformed_data)
            {
                // Release old buffer back to pool
                memory_release_buffer(internal->memory_manager, current_data);

                // Use transformed data (might be from malloc or pool)
                current_data = transformed_data;
                current_size = transformed_size;
            }
        }
    }

    *output = current_data;
    *output_size = current_size;

    pthread_mutex_unlock(&engine->mutex);
    return 0;
}