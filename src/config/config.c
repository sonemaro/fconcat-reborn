#include "config.h"
#include "../core/error.h"
#include "../core/memory.h"
#include "../../include/fconcat_api.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// FIXED: Removed fragile allocation pattern, using stack allocation
static int config_layer_init(ConfigLayer *layer, ConfigSource source, int priority)
{
    if (!layer)
        return -1;

    layer->source = source;
    layer->priority = priority;
    layer->value_capacity = 50;
    layer->values = calloc(layer->value_capacity, sizeof(ConfigValue));
    layer->value_count = 0;
    layer->source_data = NULL;

    return layer->values ? 0 : -1;
}

static void config_layer_cleanup(ConfigLayer *layer)
{
    if (!layer)
        return;

    for (int i = 0; i < layer->value_count; i++)
    {
        config_value_cleanup(&layer->values[i]);
    }
    free(layer->values);
    free(layer->source_data);

    layer->values = NULL;
    layer->value_count = 0;
    layer->value_capacity = 0;
    layer->source_data = NULL;
}

int config_layer_add_value(ConfigLayer *layer, const char *key, ConfigType type)
{
    if (!layer || !key)
        return -1;

    // Resize if needed
    if (layer->value_count >= layer->value_capacity)
    {
        int new_capacity = layer->value_capacity * 2;
        ConfigValue *new_values = realloc(layer->values, new_capacity * sizeof(ConfigValue));
        if (!new_values)
            return -1;

        layer->values = new_values;
        layer->value_capacity = new_capacity;
    }

    // Initialize new value
    ConfigValue *value = &layer->values[layer->value_count];
    if (config_value_init(value, key, type) != 0)
        return -1;

    layer->value_count++;
    return 0;
}

ConfigValue *config_layer_get_value(ConfigLayer *layer, const char *key)
{
    if (!layer || !key)
        return NULL;

    for (int i = 0; i < layer->value_count; i++)
    {
        if (layer->values[i].key && strcmp(layer->values[i].key, key) == 0)
            return &layer->values[i];
    }

    return NULL;
}

ConfigManager *config_manager_create(void)
{
    ConfigManager *manager = calloc(1, sizeof(ConfigManager));
    if (!manager)
        return NULL;

    if (pthread_mutex_init(&manager->mutex, NULL) != 0)
    {
        free(manager);
        return NULL;
    }

    manager->resolved = calloc(1, sizeof(ResolvedConfig));
    if (!manager->resolved)
    {
        pthread_mutex_destroy(&manager->mutex);
        free(manager);
        return NULL;
    }

    return manager;
}

void config_manager_destroy(ConfigManager *manager)
{
    if (!manager)
        return;

    pthread_mutex_lock(&manager->mutex);

    // Free all layers
    for (int i = 0; i < manager->layer_count; i++)
    {
        config_layer_cleanup(&manager->layers[i]);
    }

    // Free resolved config
    if (manager->resolved)
    {
        free(manager->resolved->output_format);
        free(manager->resolved->input_directory);
        free(manager->resolved->output_file);
        for (int i = 0; i < manager->resolved->exclude_count; i++)
        {
            free(manager->resolved->exclude_patterns[i]);
        }
        free(manager->resolved->exclude_patterns);

        for (int i = 0; i < manager->resolved->include_count; i++)
        {
            free(manager->resolved->include_patterns[i]);
        }
        free(manager->resolved->include_patterns);

        // Free plugin configurations
        for (int i = 0; i < manager->resolved->plugin_count; i++)
        {
            free(manager->resolved->plugins[i].path);
            for (int j = 0; j < manager->resolved->plugins[i].parameter_count; j++)
            {
                free(manager->resolved->plugins[i].parameters[j]);
            }
            free(manager->resolved->plugins[i].parameters);
        }
        free(manager->resolved->plugins);

        free(manager->resolved);
    }

    pthread_mutex_unlock(&manager->mutex);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

int config_load_defaults(ConfigManager *manager)
{
    if (!manager)
        return -1;

    pthread_mutex_lock(&manager->mutex);

    ConfigLayer *layer = &manager->layers[manager->layer_count];
    if (config_layer_init(layer, CONFIG_SOURCE_DEFAULTS, 0) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    // Use stack allocation pattern - much safer and clearer
    struct
    {
        const char *key;
        ConfigType type;
        union
        {
            int int_val;
            bool bool_val;
            const char *str_val;
        } value;
    } defaults[] = {
        {"binary_handling", CONFIG_TYPE_INT, {.int_val = BINARY_SKIP}},
        {"symlink_handling", CONFIG_TYPE_INT, {.int_val = SYMLINK_SKIP}},
        {"show_size", CONFIG_TYPE_BOOL, {.bool_val = false}},
        {"verbose", CONFIG_TYPE_BOOL, {.bool_val = false}},
        {"interactive", CONFIG_TYPE_BOOL, {.bool_val = false}},
        {"output_format", CONFIG_TYPE_STRING, {.str_val = "text"}},
        {"log_level", CONFIG_TYPE_INT, {.int_val = (int)LOG_INFO}},
    };

    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
    {
        if (config_layer_add_value(layer, defaults[i].key, defaults[i].type) != 0)
        {
            pthread_mutex_unlock(&manager->mutex);
            return -1;
        }

        ConfigValue *value = config_layer_get_value(layer, defaults[i].key);
        if (!value)
        {
            pthread_mutex_unlock(&manager->mutex);
            return -1;
        }

        switch (defaults[i].type)
        {
        case CONFIG_TYPE_INT:
            config_value_set_int(value, defaults[i].value.int_val);
            break;
        case CONFIG_TYPE_BOOL:
            config_value_set_bool(value, defaults[i].value.bool_val);
            break;
        case CONFIG_TYPE_STRING:
            config_value_set_string(value, defaults[i].value.str_val);
            break;
        default:
            break;
        }
    }

    manager->layer_count++;
    pthread_mutex_unlock(&manager->mutex);
    return 0;
}

int config_load_cli(ConfigManager *manager, int argc, char *argv[])
{
    if (!manager || argc < 3)
        return -1;

    pthread_mutex_lock(&manager->mutex);

    ConfigLayer *layer = &manager->layers[manager->layer_count];
    if (config_layer_init(layer, CONFIG_SOURCE_CLI, 100) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    // Parse basic arguments
    if (config_layer_add_value(layer, "input_directory", CONFIG_TYPE_STRING) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }
    ConfigValue *input_val = config_layer_get_value(layer, "input_directory");
    config_value_set_string(input_val, argv[1]);

    if (config_layer_add_value(layer, "output_file", CONFIG_TYPE_STRING) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }
    ConfigValue *output_val = config_layer_get_value(layer, "output_file");
    config_value_set_string(output_val, argv[2]);

    // Parse options
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "--exclude") == 0)
        {
            // Process all exclude patterns after --exclude
            i++; // Move to first pattern
            int exclude_count = 0;

            // Count patterns
            for (int j = i; j < argc && argv[j][0] != '-'; j++)
            {
                exclude_count++;
            }

            if (exclude_count > 0)
            {
                // Store exclude count
                if (config_layer_add_value(layer, "exclude_count", CONFIG_TYPE_INT) != 0)
                {
                    pthread_mutex_unlock(&manager->mutex);
                    return -1;
                }
                ConfigValue *count_val = config_layer_get_value(layer, "exclude_count");
                config_value_set_int(count_val, exclude_count);

                // Store each pattern
                for (int j = 0; j < exclude_count; j++)
                {
                    char pattern_key[64];
                    int ret = snprintf(pattern_key, sizeof(pattern_key), "exclude_pattern_%d", j);
                    if (ret < 0 || ret >= (int)sizeof(pattern_key))
                    {
                        pthread_mutex_unlock(&manager->mutex);
                        return -1;
                    }

                    if (config_layer_add_value(layer, pattern_key, CONFIG_TYPE_STRING) != 0)
                    {
                        pthread_mutex_unlock(&manager->mutex);
                        return -1;
                    }
                    ConfigValue *pattern_val = config_layer_get_value(layer, pattern_key);
                    config_value_set_string(pattern_val, argv[i + j]);
                }

                i += exclude_count - 1; // Skip processed patterns
            }
        }
        // Include pattern processing
        else if (strcmp(argv[i], "--include") == 0)
        {
            // Process all include patterns after --include
            i++; // Move to first pattern
            int include_count = 0;

            // Count patterns
            for (int j = i; j < argc && argv[j][0] != '-'; j++)
            {
                include_count++;
            }

            if (include_count > 0)
            {
                // Store include count
                if (config_layer_add_value(layer, "include_count", CONFIG_TYPE_INT) != 0)
                {
                    pthread_mutex_unlock(&manager->mutex);
                    return -1;
                }
                ConfigValue *count_val = config_layer_get_value(layer, "include_count");
                config_value_set_int(count_val, include_count);

                // Store each pattern
                for (int j = 0; j < include_count; j++)
                {
                    char pattern_key[64];
                    int ret = snprintf(pattern_key, sizeof(pattern_key), "include_pattern_%d", j);
                    if (ret < 0 || ret >= (int)sizeof(pattern_key))
                    {
                        pthread_mutex_unlock(&manager->mutex);
                        return -1;
                    }

                    if (config_layer_add_value(layer, pattern_key, CONFIG_TYPE_STRING) != 0)
                    {
                        pthread_mutex_unlock(&manager->mutex);
                        return -1;
                    }
                    ConfigValue *pattern_val = config_layer_get_value(layer, pattern_key);
                    config_value_set_string(pattern_val, argv[i + j]);
                }

                i += include_count - 1; // Skip processed patterns
            }
        }
        else if (strcmp(argv[i], "--show-size") == 0 || strcmp(argv[i], "-s") == 0)
        {
            if (config_layer_add_value(layer, "show_size", CONFIG_TYPE_BOOL) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "show_size");
            config_value_set_bool(val, true);
        }
        else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
        {
            if (config_layer_add_value(layer, "verbose", CONFIG_TYPE_BOOL) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "verbose");
            config_value_set_bool(val, true);

            // Set log level to DEBUG for verbose mode
            if (config_layer_add_value(layer, "log_level", CONFIG_TYPE_INT) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *log_val = config_layer_get_value(layer, "log_level");
            config_value_set_int(log_val, (int)LOG_DEBUG);
        }
        else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
        {
            if (config_layer_add_value(layer, "log_level", CONFIG_TYPE_INT) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "log_level");
            i++;

            // Parse log level
            int log_level = (int)LOG_INFO; // Default
            if (strcmp(argv[i], "error") == 0)
                log_level = (int)LOG_ERROR;
            else if (strcmp(argv[i], "warning") == 0)
                log_level = (int)LOG_WARNING;
            else if (strcmp(argv[i], "info") == 0)
                log_level = (int)LOG_INFO;
            else if (strcmp(argv[i], "debug") == 0)
                log_level = (int)LOG_DEBUG;
            else if (strcmp(argv[i], "trace") == 0)
                log_level = (int)LOG_TRACE;

            config_value_set_int(val, log_level);
        }
        else if (strcmp(argv[i], "--interactive") == 0)
        {
            if (config_layer_add_value(layer, "interactive", CONFIG_TYPE_BOOL) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "interactive");
            config_value_set_bool(val, true);
        }
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc)
        {
            if (config_layer_add_value(layer, "output_format", CONFIG_TYPE_STRING) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "output_format");
            config_value_set_string(val, argv[++i]);
        }
        else if (strcmp(argv[i], "--binary-skip") == 0)
        {
            if (config_layer_add_value(layer, "binary_handling", CONFIG_TYPE_INT) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "binary_handling");
            config_value_set_int(val, BINARY_SKIP);
        }
        else if (strcmp(argv[i], "--binary-include") == 0)
        {
            if (config_layer_add_value(layer, "binary_handling", CONFIG_TYPE_INT) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "binary_handling");
            config_value_set_int(val, BINARY_INCLUDE);
        }
        else if (strcmp(argv[i], "--binary-placeholder") == 0)
        {
            if (config_layer_add_value(layer, "binary_handling", CONFIG_TYPE_INT) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "binary_handling");
            config_value_set_int(val, BINARY_PLACEHOLDER);
        }
        else if (strcmp(argv[i], "--symlinks") == 0 && i + 1 < argc)
        {
            if (config_layer_add_value(layer, "symlink_handling", CONFIG_TYPE_INT) != 0)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *val = config_layer_get_value(layer, "symlink_handling");
            i++;
            if (strcmp(argv[i], "skip") == 0)
            {
                config_value_set_int(val, SYMLINK_SKIP);
            }
            else if (strcmp(argv[i], "follow") == 0)
            {
                config_value_set_int(val, SYMLINK_FOLLOW);
            }
            else if (strcmp(argv[i], "include") == 0)
            {
                config_value_set_int(val, SYMLINK_INCLUDE);
            }
            else if (strcmp(argv[i], "placeholder") == 0)
            {
                config_value_set_int(val, SYMLINK_PLACEHOLDER);
            }
        }
        else if (strcmp(argv[i], "--plugin") == 0 && i + 1 < argc)
        {
            // Get current plugin count
            int plugin_count = config_get_int(manager, "plugin_count");

            // Add plugin count if it doesn't exist
            if (plugin_count == 0)
            {
                if (config_layer_add_value(layer, "plugin_count", CONFIG_TYPE_INT) != 0)
                {
                    pthread_mutex_unlock(&manager->mutex);
                    return -1;
                }
                ConfigValue *count_val = config_layer_get_value(layer, "plugin_count");
                config_value_set_int(count_val, 1);
                plugin_count = 1;
            }
            else
            {
                // Increment plugin count
                ConfigValue *count_val = config_layer_get_value(layer, "plugin_count");
                if (count_val)
                {
                    config_value_set_int(count_val, plugin_count + 1);
                    plugin_count++;
                }
                else
                {
                    // Create plugin count if it doesn't exist
                    if (config_layer_add_value(layer, "plugin_count", CONFIG_TYPE_INT) != 0)
                    {
                        pthread_mutex_unlock(&manager->mutex);
                        return -1;
                    }
                    ConfigValue *count_val = config_layer_get_value(layer, "plugin_count");
                    config_value_set_int(count_val, 1);
                    plugin_count = 1;
                }
            }

            // Parse plugin specification: path[:param1=value1,param2=value2,...]
            i++; // Move to plugin spec
            char *plugin_spec = strdup(argv[i]);
            if (!plugin_spec)
            {
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }

            // Split path and parameters
            char *path_part = plugin_spec;
            char *params_part = strchr(plugin_spec, ':');
            if (params_part)
            {
                *params_part = '\0'; // Terminate path part
                params_part++;       // Move to parameters part
            }

            // Store plugin path
            char plugin_path_key[64];
            int ret = snprintf(plugin_path_key, sizeof(plugin_path_key), "plugin_path_%d", plugin_count - 1);
            if (ret < 0 || ret >= (int)sizeof(plugin_path_key))
            {
                free(plugin_spec);
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }

            if (config_layer_add_value(layer, plugin_path_key, CONFIG_TYPE_STRING) != 0)
            {
                free(plugin_spec);
                pthread_mutex_unlock(&manager->mutex);
                return -1;
            }
            ConfigValue *path_val = config_layer_get_value(layer, plugin_path_key);
            config_value_set_string(path_val, path_part);

            // Parse and store parameters if present
            if (params_part && strlen(params_part) > 0)
            {
                char param_count_key[64];
                ret = snprintf(param_count_key, sizeof(param_count_key), "plugin_param_count_%d", plugin_count - 1);
                if (ret >= 0 && ret < (int)sizeof(param_count_key))
                {
                    // Count parameters
                    int param_count = 0;
                    char *params_copy = strdup(params_part);
                    if (params_copy)
                    {
                        char *param = strtok(params_copy, ",");
                        while (param && param_count < MAX_PLUGIN_PARAMS)
                        {
                            param_count++;
                            param = strtok(NULL, ",");
                        }
                        free(params_copy);

                        // Store parameter count
                        if (config_layer_add_value(layer, param_count_key, CONFIG_TYPE_INT) != 0)
                        {
                            free(plugin_spec);
                            pthread_mutex_unlock(&manager->mutex);
                            return -1;
                        }
                        ConfigValue *param_count_val = config_layer_get_value(layer, param_count_key);
                        config_value_set_int(param_count_val, param_count);

                        // Store each parameter
                        params_copy = strdup(params_part);
                        if (params_copy)
                        {
                            char *param = strtok(params_copy, ",");
                            int param_idx = 0;
                            while (param && param_idx < param_count)
                            {
                                char param_key[64];
                                ret = snprintf(param_key, sizeof(param_key), "plugin_param_%d_%d", plugin_count - 1, param_idx);
                                if (ret >= 0 && ret < (int)sizeof(param_key))
                                {
                                    if (config_layer_add_value(layer, param_key, CONFIG_TYPE_STRING) != 0)
                                    {
                                        free(params_copy);
                                        free(plugin_spec);
                                        pthread_mutex_unlock(&manager->mutex);
                                        return -1;
                                    }
                                    ConfigValue *param_val = config_layer_get_value(layer, param_key);
                                    config_value_set_string(param_val, param);
                                }
                                param = strtok(NULL, ",");
                                param_idx++;
                            }
                            free(params_copy);
                        }
                    }
                }
            }

            free(plugin_spec);
        }
        // Add more options as needed
    }

    manager->layer_count++;
    pthread_mutex_unlock(&manager->mutex);
    return 0;
}

ResolvedConfig *config_resolve(ConfigManager *manager)
{
    if (!manager || !manager->resolved)
        return NULL;

    pthread_mutex_lock(&manager->mutex);

    ResolvedConfig *config = manager->resolved;

    // Resolve basic configuration
    config->binary_handling = config_get_int(manager, "binary_handling");
    config->symlink_handling = config_get_int(manager, "symlink_handling");
    config->show_size = config_get_bool(manager, "show_size");
    config->verbose = config_get_bool(manager, "verbose");
    config->interactive = config_get_bool(manager, "interactive");
    config->log_level = config_get_int(manager, "log_level");

    const char *format = config_get_string(manager, "output_format");
    if (format)
    {
        free(config->output_format);
        config->output_format = strdup(format);
        if (!config->output_format) {
            free(config);
            return NULL;  // Allocation failed
        }
    }

    const char *input_dir = config_get_string(manager, "input_directory");
    if (input_dir)
    {
        free(config->input_directory);
        config->input_directory = strdup(input_dir);
        if (!config->input_directory) {
            free(config->output_format);
            free(config);
            return NULL;  // Allocation failed
        }
    }

    const char *output_file = config_get_string(manager, "output_file");
    if (output_file)
    {
        free(config->output_file);
        config->output_file = strdup(output_file);
        if (!config->output_file) {
            free(config->output_format);
            free(config->input_directory);
            free(config);
            return NULL;  // Allocation failed
        }
    }

    // Resolve exclude patterns
    int exclude_count = config_get_int(manager, "exclude_count");
    if (exclude_count > 0)
    {
        // Free existing patterns
        for (int i = 0; i < config->exclude_count; i++)
        {
            free(config->exclude_patterns[i]);
        }
        free(config->exclude_patterns);

        config->exclude_patterns = malloc(exclude_count * sizeof(char *));
        if (config->exclude_patterns)
        {
            config->exclude_count = exclude_count;

            for (int i = 0; i < exclude_count; i++)
            {
                char pattern_key[64];
                int ret = snprintf(pattern_key, sizeof(pattern_key), "exclude_pattern_%d", i);
                if (ret < 0 || ret >= (int)sizeof(pattern_key))
                {
                    config->exclude_patterns[i] = strdup("");
                    if (!config->exclude_patterns[i]) {
                        // Truncate pattern list on allocation failure
                        config->exclude_count = i;
                        break;
                    }
                    continue;
                }

                const char *pattern = config_get_string(manager, pattern_key);
                if (pattern)
                {
                    config->exclude_patterns[i] = strdup(pattern);
                }
                else
                {
                    config->exclude_patterns[i] = strdup("");
                }
                if (!config->exclude_patterns[i]) {
                    // Truncate pattern list on allocation failure
                    config->exclude_count = i;
                    break;
                }
            }
        }
        else
        {
            config->exclude_count = 0;
        }
    }

    // Resolve include patterns
    int include_count = config_get_int(manager, "include_count");
    if (include_count > 0)
    {
        // Free existing patterns
        for (int i = 0; i < config->include_count; i++)
        {
            free(config->include_patterns[i]);
        }
        free(config->include_patterns);

        config->include_patterns = malloc(include_count * sizeof(char *));
        if (config->include_patterns)
        {
            config->include_count = include_count;

            for (int i = 0; i < include_count; i++)
            {
                char pattern_key[64];
                int ret = snprintf(pattern_key, sizeof(pattern_key), "include_pattern_%d", i);
                if (ret < 0 || ret >= (int)sizeof(pattern_key))
                {
                    config->include_patterns[i] = strdup("");
                    if (!config->include_patterns[i]) {
                        // Truncate pattern list on allocation failure
                        config->include_count = i;
                        break;
                    }
                    continue;
                }

                const char *pattern = config_get_string(manager, pattern_key);
                if (pattern)
                {
                    config->include_patterns[i] = strdup(pattern);
                }
                else
                {
                    config->include_patterns[i] = strdup("");
                }
                if (!config->include_patterns[i]) {
                    // Truncate pattern list on allocation failure
                    config->include_count = i;
                    break;
                }
            }
        }
        else
        {
            config->include_count = 0;
        }
    }

    // Resolve plugin configurations
    int plugin_count = config_get_int(manager, "plugin_count");
    if (plugin_count > 0)
    {
        // Free existing plugin configurations
        for (int i = 0; i < config->plugin_count; i++)
        {
            free(config->plugins[i].path);
            for (int j = 0; j < config->plugins[i].parameter_count; j++)
            {
                free(config->plugins[i].parameters[j]);
            }
            free(config->plugins[i].parameters);
        }
        free(config->plugins);

        config->plugins = calloc(plugin_count, sizeof(PluginConfig));
        if (config->plugins)
        {
            config->plugin_count = plugin_count;

            for (int i = 0; i < plugin_count; i++)
            {
                // Get plugin path
                char plugin_path_key[64];
                int ret = snprintf(plugin_path_key, sizeof(plugin_path_key), "plugin_path_%d", i);
                if (ret >= 0 && ret < (int)sizeof(plugin_path_key))
                {
                    const char *plugin_path = config_get_string(manager, plugin_path_key);
                    if (plugin_path)
                    {
                        config->plugins[i].path = strdup(plugin_path);
                    }
                }

                // Get plugin parameters
                char param_count_key[64];
                ret = snprintf(param_count_key, sizeof(param_count_key), "plugin_param_count_%d", i);
                if (ret >= 0 && ret < (int)sizeof(param_count_key))
                {
                    int param_count = config_get_int(manager, param_count_key);
                    if (param_count > 0)
                    {
                        config->plugins[i].parameters = malloc(param_count * sizeof(char *));
                        if (config->plugins[i].parameters)
                        {
                            config->plugins[i].parameter_count = param_count;

                            for (int j = 0; j < param_count; j++)
                            {
                                char param_key[64];
                                ret = snprintf(param_key, sizeof(param_key), "plugin_param_%d_%d", i, j);
                                if (ret >= 0 && ret < (int)sizeof(param_key))
                                {
                                    const char *param = config_get_string(manager, param_key);
                                    if (param)
                                    {
                                        config->plugins[i].parameters[j] = strdup(param);
                                    }
                                    else
                                    {
                                        config->plugins[i].parameters[j] = strdup("");
                                    }
                                }
                                else
                                {
                                    config->plugins[i].parameters[j] = strdup("");
                                }
                            }
                        }
                    }
                }
            }
        }
        else
        {
            config->plugin_count = 0;
        }
    }
    else
    {
        // No plugins specified, make sure plugin_count is 0
        config->plugin_count = 0;
        if (config->plugins)
        {
            free(config->plugins);
            config->plugins = NULL;
        }
    }

    pthread_mutex_unlock(&manager->mutex);
    return config;
}

const char *config_get_string(ConfigManager *manager, const char *key)
{
    if (!manager || !key)
        return NULL;

    // Search from highest to lowest priority
    for (int i = manager->layer_count - 1; i >= 0; i--)
    {
        ConfigLayer *layer = &manager->layers[i];
        for (int j = 0; j < layer->value_count; j++)
        {
            ConfigValue *val = &layer->values[j];
            if (val->key && strcmp(val->key, key) == 0 && val->type == CONFIG_TYPE_STRING)
            {
                return val->value.string_value;
            }
        }
    }

    return NULL;
}

int config_get_int(ConfigManager *manager, const char *key)
{
    if (!manager || !key)
        return 0;

    // Search from highest to lowest priority
    for (int i = manager->layer_count - 1; i >= 0; i--)
    {
        ConfigLayer *layer = &manager->layers[i];
        for (int j = 0; j < layer->value_count; j++)
        {
            ConfigValue *val = &layer->values[j];
            if (val->key && strcmp(val->key, key) == 0 && val->type == CONFIG_TYPE_INT)
            {
                return val->value.int_value;
            }
        }
    }

    return 0;
}

bool config_get_bool(ConfigManager *manager, const char *key)
{
    if (!manager || !key)
        return false;

    // Search from highest to lowest priority
    for (int i = manager->layer_count - 1; i >= 0; i--)
    {
        ConfigLayer *layer = &manager->layers[i];
        for (int j = 0; j < layer->value_count; j++)
        {
            ConfigValue *val = &layer->values[j];
            if (val->key && strcmp(val->key, key) == 0 && val->type == CONFIG_TYPE_BOOL)
            {
                return val->value.bool_value;
            }
        }
    }

    return false;
}

// Configuration value functions
int config_value_init(ConfigValue *value, const char *key, ConfigType type)
{
    if (!value || !key)
        return -1;

    value->key = strdup(key);
    if (!value->key)
        return -1;

    value->type = type;

    // Initialize the value union
    switch (type)
    {
    case CONFIG_TYPE_STRING:
        value->value.string_value = NULL;
        break;
    case CONFIG_TYPE_INT:
        value->value.int_value = 0;
        break;
    case CONFIG_TYPE_BOOL:
        value->value.bool_value = false;
        break;
    case CONFIG_TYPE_FLOAT:
        value->value.float_value = 0.0;
        break;
    }

    return 0;
}

void config_value_cleanup(ConfigValue *value)
{
    if (!value)
        return;

    free(value->key);
    value->key = NULL;

    if (value->type == CONFIG_TYPE_STRING)
    {
        free(value->value.string_value);
        value->value.string_value = NULL;
    }
}

void config_value_set_string(ConfigValue *value, const char *str)
{
    if (!value || value->type != CONFIG_TYPE_STRING)
        return;

    free(value->value.string_value);
    value->value.string_value = str ? strdup(str) : NULL;
}

void config_value_set_int(ConfigValue *value, int val)
{
    if (!value || value->type != CONFIG_TYPE_INT)
        return;

    value->value.int_value = val;
}

void config_value_set_bool(ConfigValue *value, bool val)
{
    if (!value || value->type != CONFIG_TYPE_BOOL)
        return;

    value->value.bool_value = val;
}

void config_value_set_float(ConfigValue *value, double val)
{
    if (!value || value->type != CONFIG_TYPE_FLOAT)
        return;

    value->value.float_value = val;
}