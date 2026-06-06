#include "config.h"
#include "../../include/fconcat_api.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int config_value_set_string_checked(ConfigValue *value, const char *str);

static int config_layer_init(ConfigLayer *layer, ConfigSource source, int priority)
{
    if (!layer)
        return -1;

    memset(layer, 0, sizeof(*layer));
    layer->source = source;
    layer->priority = priority;
    layer->value_capacity = 32;
    layer->values = calloc((size_t)layer->value_capacity, sizeof(ConfigValue));
    return layer->values ? 0 : -1;
}

static void config_layer_cleanup(ConfigLayer *layer)
{
    if (!layer)
        return;

    for (int i = 0; i < layer->value_count; i++)
        config_value_cleanup(&layer->values[i]);

    free(layer->values);
    free(layer->source_data);
    memset(layer, 0, sizeof(*layer));
}

static int parse_positive_int(const char *value, int min, int max, int *out)
{
    if (!value || !out)
        return -1;
    if (value[0] == '\0')
        return -1;
    for (const char *p = value; *p; p++)
    {
        if (*p < '0' || *p > '9')
            return -1;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < min || parsed > max)
        return -1;

    *out = (int)parsed;
    return 0;
}

static int parse_log_level(const char *value, int *out)
{
    if (!value || !out)
        return -1;

    if (strcmp(value, "error") == 0)
        *out = (int)LOG_ERROR;
    else if (strcmp(value, "warning") == 0)
        *out = (int)LOG_WARNING;
    else if (strcmp(value, "info") == 0)
        *out = (int)LOG_INFO;
    else if (strcmp(value, "debug") == 0)
        *out = (int)LOG_DEBUG;
    else if (strcmp(value, "trace") == 0)
        *out = (int)LOG_TRACE;
    else
        return -1;

    return 0;
}

static int add_or_set_int(ConfigLayer *layer, const char *key, int value)
{
    ConfigValue *existing = config_layer_get_value(layer, key);
    if (!existing)
    {
        if (config_layer_add_value(layer, key, CONFIG_TYPE_INT) != 0)
            return -1;
        existing = config_layer_get_value(layer, key);
    }
    if (!existing)
        return -1;
    config_value_set_int(existing, value);
    return 0;
}

static int add_or_set_bool(ConfigLayer *layer, const char *key, bool value)
{
    ConfigValue *existing = config_layer_get_value(layer, key);
    if (!existing)
    {
        if (config_layer_add_value(layer, key, CONFIG_TYPE_BOOL) != 0)
            return -1;
        existing = config_layer_get_value(layer, key);
    }
    if (!existing)
        return -1;
    config_value_set_bool(existing, value);
    return 0;
}

static int add_or_set_string(ConfigLayer *layer, const char *key, const char *value)
{
    ConfigValue *existing = config_layer_get_value(layer, key);
    if (!existing)
    {
        if (config_layer_add_value(layer, key, CONFIG_TYPE_STRING) != 0)
            return -1;
        existing = config_layer_get_value(layer, key);
    }
    if (!existing)
        return -1;
    return config_value_set_string_checked(existing, value);
}

static int append_indexed_string(ConfigLayer *layer, const char *count_key,
                                 const char *item_prefix, const char *value,
                                 int max_items)
{
    int count = 0;
    ConfigValue *count_val = config_layer_get_value(layer, count_key);
    if (count_val)
        count = count_val->value.int_value;

    if (count < 0 || count >= max_items)
        return -1;

    char key[64];
    int n = snprintf(key, sizeof(key), "%s_%d", item_prefix, count);
    if (n < 0 || (size_t)n >= sizeof(key))
        return -1;

    if (add_or_set_string(layer, key, value) != 0)
        return -1;
    return add_or_set_int(layer, count_key, count + 1);
}

static int split_list_option(ConfigLayer *layer, int argc, char *argv[], int *index,
                             const char *count_key, const char *item_prefix,
                             int max_items)
{
    int i = *index + 1;
    int added = 0;
    while (i < argc && argv[i][0] != '-')
    {
        if (append_indexed_string(layer, count_key, item_prefix, argv[i], max_items) != 0)
            return -1;
        added++;
        i++;
    }

    if (added == 0)
        return -1;

    *index = i - 1;
    return 0;
}

static int parse_listen_value(const char *listen, char **host_out, int *port_out)
{
    if (!listen || !host_out || !port_out)
        return -1;

    const char *colon = strrchr(listen, ':');
    if (!colon || colon == listen || colon[1] == '\0')
        return -1;

    size_t host_len = (size_t)(colon - listen);
    char *host = malloc(host_len + 1);
    if (!host)
        return -1;
    memcpy(host, listen, host_len);
    host[host_len] = '\0';

    int port = 0;
    if (parse_positive_int(colon + 1, 1, 65535, &port) != 0)
    {
        free(host);
        return -1;
    }

    *host_out = host;
    *port_out = port;
    return 0;
}

void resolved_config_cleanup(ResolvedConfig *config)
{
    if (!config)
        return;

    free(config->input_directory);
    free(config->output_file);
    for (int i = 0; i < config->exclude_count; i++)
        free(config->exclude_patterns[i]);
    free(config->exclude_patterns);
    for (int i = 0; i < config->include_count; i++)
        free(config->include_patterns[i]);
    free(config->include_patterns);
    free(config->listen_host);
    for (int i = 0; i < config->allow_root_count; i++)
        free(config->allow_roots[i]);
    free(config->allow_roots);
    free(config->auth_token);
    memset(config, 0, sizeof(*config));
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
    for (int i = 0; i < manager->layer_count; i++)
        config_layer_cleanup(&manager->layers[i]);
    resolved_config_cleanup(manager->resolved);
    free(manager->resolved);
    pthread_mutex_unlock(&manager->mutex);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

int config_load_defaults(ConfigManager *manager)
{
    if (!manager)
        return -1;

    pthread_mutex_lock(&manager->mutex);
    if (manager->layer_count < 0 || manager->layer_count >= MAX_CONFIG_LAYERS)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    ConfigLayer *layer = &manager->layers[manager->layer_count];
    if (config_layer_init(layer, CONFIG_SOURCE_DEFAULTS, 0) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    int ok = 0;
    ok |= add_or_set_int(layer, "mode", (int)FCONCAT_MODE_BATCH);
    ok |= add_or_set_int(layer, "binary_handling", (int)BINARY_SKIP);
    ok |= add_or_set_int(layer, "symlink_handling", (int)SYMLINK_SKIP);
    ok |= add_or_set_bool(layer, "show_size", false);
    ok |= add_or_set_bool(layer, "verbose", false);
    ok |= add_or_set_int(layer, "log_level", (int)LOG_INFO);
    ok |= add_or_set_string(layer, "listen", "127.0.0.1:8080");
    ok |= add_or_set_int(layer, "server_workers", DEFAULT_SERVER_WORKERS);
    ok |= add_or_set_int(layer, "server_queue_size", DEFAULT_SERVER_QUEUE);

    if (ok != 0)
    {
        config_layer_cleanup(layer);
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    manager->layer_count++;
    pthread_mutex_unlock(&manager->mutex);
    return 0;
}

int config_load_cli(ConfigManager *manager, int argc, char *argv[])
{
    if (!manager || argc < 2 || !argv)
        return -1;
    for (int i = 0; i < argc; i++)
    {
        if (!argv[i])
            return -1;
    }

    pthread_mutex_lock(&manager->mutex);
    if (manager->layer_count < 0 || manager->layer_count >= MAX_CONFIG_LAYERS)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    ConfigLayer *layer = &manager->layers[manager->layer_count];
    if (config_layer_init(layer, CONFIG_SOURCE_CLI, 100) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    int result = 0;
    if (strcmp(argv[1], "--serve") == 0)
    {
        result |= add_or_set_int(layer, "mode", (int)FCONCAT_MODE_SERVER);
        for (int i = 2; result == 0 && i < argc; i++)
        {
            if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc)
            {
                result |= add_or_set_string(layer, "listen", argv[++i]);
            }
            else if (strcmp(argv[i], "--allow-root") == 0 && i + 1 < argc)
            {
                result |= append_indexed_string(layer, "allow_root_count", "allow_root", argv[++i], MAX_ALLOW_ROOTS);
            }
            else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc)
            {
                int workers = 0;
                result |= parse_positive_int(argv[++i], 1, 256, &workers);
                result |= add_or_set_int(layer, "server_workers", workers);
            }
            else if (strcmp(argv[i], "--queue") == 0 && i + 1 < argc)
            {
                int queue_size = 0;
                result |= parse_positive_int(argv[++i], 1, 4096, &queue_size);
                result |= add_or_set_int(layer, "server_queue_size", queue_size);
            }
            else if (strcmp(argv[i], "--auth-token") == 0 && i + 1 < argc)
            {
                result |= add_or_set_string(layer, "auth_token", argv[++i]);
            }
            else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
            {
                result |= add_or_set_bool(layer, "verbose", true);
                result |= add_or_set_int(layer, "log_level", (int)LOG_DEBUG);
            }
            else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
            {
                int level = 0;
                result |= parse_log_level(argv[++i], &level);
                result |= add_or_set_int(layer, "log_level", level);
            }
            else
            {
                result = -1;
            }
        }
    }
    else if (argc >= 3)
    {
        result |= add_or_set_int(layer, "mode", (int)FCONCAT_MODE_BATCH);
        result |= add_or_set_string(layer, "input_directory", argv[1]);
        result |= add_or_set_string(layer, "output_file", argv[2]);

        for (int i = 3; result == 0 && i < argc; i++)
        {
            if (strcmp(argv[i], "--include") == 0)
            {
                result |= split_list_option(layer, argc, argv, &i, "include_count", "include_pattern", MAX_INCLUDES);
            }
            else if (strcmp(argv[i], "--exclude") == 0)
            {
                result |= split_list_option(layer, argc, argv, &i, "exclude_count", "exclude_pattern", MAX_EXCLUDES);
            }
            else if (strcmp(argv[i], "--show-size") == 0 || strcmp(argv[i], "-s") == 0)
            {
                result |= add_or_set_bool(layer, "show_size", true);
            }
            else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
            {
                result |= add_or_set_bool(layer, "verbose", true);
                result |= add_or_set_int(layer, "log_level", (int)LOG_DEBUG);
            }
            else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
            {
                int level = 0;
                result |= parse_log_level(argv[++i], &level);
                result |= add_or_set_int(layer, "log_level", level);
            }
            else if (strcmp(argv[i], "--binary-skip") == 0)
            {
                result |= add_or_set_int(layer, "binary_handling", (int)BINARY_SKIP);
            }
            else if (strcmp(argv[i], "--binary-include") == 0)
            {
                result |= add_or_set_int(layer, "binary_handling", (int)BINARY_INCLUDE);
            }
            else if (strcmp(argv[i], "--binary-placeholder") == 0)
            {
                result |= add_or_set_int(layer, "binary_handling", (int)BINARY_PLACEHOLDER);
            }
            else if (strcmp(argv[i], "--symlinks") == 0 && i + 1 < argc)
            {
                i++;
                if (strcmp(argv[i], "skip") == 0)
                    result |= add_or_set_int(layer, "symlink_handling", (int)SYMLINK_SKIP);
                else if (strcmp(argv[i], "follow") == 0)
                    result |= add_or_set_int(layer, "symlink_handling", (int)SYMLINK_FOLLOW);
                else if (strcmp(argv[i], "include") == 0)
                    result |= add_or_set_int(layer, "symlink_handling", (int)SYMLINK_INCLUDE);
                else if (strcmp(argv[i], "placeholder") == 0)
                    result |= add_or_set_int(layer, "symlink_handling", (int)SYMLINK_PLACEHOLDER);
                else
                    result = -1;
            }
            else
            {
                result = -1;
            }
        }
    }
    else
    {
        result = -1;
    }

    if (result != 0)
    {
        config_layer_cleanup(layer);
        pthread_mutex_unlock(&manager->mutex);
        return -1;
    }

    manager->layer_count++;
    pthread_mutex_unlock(&manager->mutex);
    return 0;
}

static char **resolve_string_array(ConfigManager *manager, const char *count_key,
                                   const char *item_prefix, int max_count, int *out_count)
{
    if (!manager || !count_key || !item_prefix || !out_count)
        return NULL;

    int count = config_get_int(manager, count_key);
    *out_count = 0;
    if (count <= 0)
        return NULL;
    if (count > max_count)
        return NULL;

    char **items = calloc((size_t)count, sizeof(char *));
    if (!items)
        return NULL;

    for (int i = 0; i < count; i++)
    {
        char key[64];
        int n = snprintf(key, sizeof(key), "%s_%d", item_prefix, i);
        if (n < 0 || (size_t)n >= sizeof(key))
            goto fail;

        const char *value = config_get_string(manager, key);
        items[i] = strdup(value ? value : "");
        if (!items[i])
            goto fail;
    }

    *out_count = count;
    return items;

fail:
    for (int i = 0; i < count; i++)
        free(items[i]);
    free(items);
    return NULL;
}

static ResolvedConfig *config_resolve_fail(ConfigManager *manager, ResolvedConfig *config)
{
    resolved_config_cleanup(config);
    pthread_mutex_unlock(&manager->mutex);
    return NULL;
}

ResolvedConfig *config_resolve(ConfigManager *manager)
{
    if (!manager || !manager->resolved)
        return NULL;

    pthread_mutex_lock(&manager->mutex);
    if (manager->layer_count < 0 || manager->layer_count > MAX_CONFIG_LAYERS)
    {
        pthread_mutex_unlock(&manager->mutex);
        return NULL;
    }

    ResolvedConfig *config = manager->resolved;
    resolved_config_cleanup(config);

    config->mode = (FconcatMode)config_get_int(manager, "mode");
    config->binary_handling = (BinaryHandling)config_get_int(manager, "binary_handling");
    config->symlink_handling = (SymlinkHandling)config_get_int(manager, "symlink_handling");
    config->show_size = config_get_bool(manager, "show_size");
    config->verbose = config_get_bool(manager, "verbose");
    config->log_level = config_get_int(manager, "log_level");
    config->server_workers = config_get_int(manager, "server_workers");
    config->server_queue_size = config_get_int(manager, "server_queue_size");

    const char *input = config_get_string(manager, "input_directory");
    const char *output = config_get_string(manager, "output_file");
    const char *token = config_get_string(manager, "auth_token");

    if (input)
        config->input_directory = strdup(input);
    if (output)
        config->output_file = strdup(output);
    if (token)
        config->auth_token = strdup(token);

    config->include_patterns = resolve_string_array(manager, "include_count", "include_pattern", MAX_INCLUDES, &config->include_count);
    config->exclude_patterns = resolve_string_array(manager, "exclude_count", "exclude_pattern", MAX_EXCLUDES, &config->exclude_count);
    config->allow_roots = resolve_string_array(manager, "allow_root_count", "allow_root", MAX_ALLOW_ROOTS, &config->allow_root_count);

    const char *listen = config_get_string(manager, "listen");
    if (parse_listen_value(listen ? listen : "127.0.0.1:8080", &config->listen_host, &config->listen_port) != 0)
        return config_resolve_fail(manager, config);

    if ((input && !config->input_directory) || (output && !config->output_file) ||
        (token && !config->auth_token) ||
        (config_get_int(manager, "include_count") > 0 && !config->include_patterns) ||
        (config_get_int(manager, "exclude_count") > 0 && !config->exclude_patterns) ||
        (config_get_int(manager, "allow_root_count") > 0 && !config->allow_roots))
        return config_resolve_fail(manager, config);

    if (config->mode == FCONCAT_MODE_BATCH && (!config->input_directory || !config->output_file))
        return config_resolve_fail(manager, config);

    if (config->mode == FCONCAT_MODE_SERVER && config->allow_root_count <= 0)
        return config_resolve_fail(manager, config);

    pthread_mutex_unlock(&manager->mutex);
    return config;
}

const char *config_get_string(ConfigManager *manager, const char *key)
{
    if (!manager || !key)
        return NULL;

    for (int i = manager->layer_count - 1; i >= 0; i--)
    {
        ConfigLayer *layer = &manager->layers[i];
        for (int j = 0; j < layer->value_count; j++)
        {
            ConfigValue *value = &layer->values[j];
            if (value->key && strcmp(value->key, key) == 0 && value->type == CONFIG_TYPE_STRING)
                return value->value.string_value;
        }
    }

    return NULL;
}

int config_get_int(ConfigManager *manager, const char *key)
{
    if (!manager || !key)
        return 0;

    for (int i = manager->layer_count - 1; i >= 0; i--)
    {
        ConfigLayer *layer = &manager->layers[i];
        for (int j = 0; j < layer->value_count; j++)
        {
            ConfigValue *value = &layer->values[j];
            if (value->key && strcmp(value->key, key) == 0 && value->type == CONFIG_TYPE_INT)
                return value->value.int_value;
        }
    }

    return 0;
}

bool config_get_bool(ConfigManager *manager, const char *key)
{
    if (!manager || !key)
        return false;

    for (int i = manager->layer_count - 1; i >= 0; i--)
    {
        ConfigLayer *layer = &manager->layers[i];
        for (int j = 0; j < layer->value_count; j++)
        {
            ConfigValue *value = &layer->values[j];
            if (value->key && strcmp(value->key, key) == 0 && value->type == CONFIG_TYPE_BOOL)
                return value->value.bool_value;
        }
    }

    return false;
}

int config_value_init(ConfigValue *value, const char *key, ConfigType type)
{
    if (!value || !key)
        return -1;

    memset(value, 0, sizeof(*value));
    value->key = strdup(key);
    if (!value->key)
        return -1;
    value->type = type;
    return 0;
}

void config_value_cleanup(ConfigValue *value)
{
    if (!value)
        return;

    free(value->key);
    if (value->type == CONFIG_TYPE_STRING)
        free(value->value.string_value);
    memset(value, 0, sizeof(*value));
}

static int config_value_set_string_checked(ConfigValue *value, const char *str)
{
    if (!value || value->type != CONFIG_TYPE_STRING)
        return -1;

    char *copy = NULL;
    if (str)
    {
        copy = strdup(str);
        if (!copy)
            return -1;
    }
    free(value->value.string_value);
    value->value.string_value = copy;
    return 0;
}

void config_value_set_string(ConfigValue *value, const char *str)
{
    (void)config_value_set_string_checked(value, str);
}

void config_value_set_int(ConfigValue *value, int val)
{
    if (value && value->type == CONFIG_TYPE_INT)
        value->value.int_value = val;
}

void config_value_set_bool(ConfigValue *value, bool val)
{
    if (value && value->type == CONFIG_TYPE_BOOL)
        value->value.bool_value = val;
}

int config_layer_add_value(ConfigLayer *layer, const char *key, ConfigType type)
{
    if (!layer || !key)
        return -1;
    if (layer->value_count < 0 || layer->value_capacity < 0 ||
        layer->value_count > layer->value_capacity)
        return -1;
    if (!layer->values && layer->value_capacity > 0)
        return -1;

    if (layer->value_count >= layer->value_capacity)
    {
        if (layer->value_capacity > INT_MAX / 2)
            return -1;
        int new_capacity = layer->value_capacity > 0 ? layer->value_capacity * 2 : 32;
        ConfigValue *new_values = realloc(layer->values, (size_t)new_capacity * sizeof(ConfigValue));
        if (!new_values)
            return -1;
        memset(new_values + layer->value_capacity, 0,
               (size_t)(new_capacity - layer->value_capacity) * sizeof(ConfigValue));
        layer->values = new_values;
        layer->value_capacity = new_capacity;
    }

    if (config_value_init(&layer->values[layer->value_count], key, type) != 0)
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
