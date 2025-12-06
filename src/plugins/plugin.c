#include "plugin.h"
#include "../core/error.h"
#include "../core/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define dlopen(lib, flags) LoadLibraryA(lib)
#define dlsym(handle, symbol) GetProcAddress((HMODULE)(handle), symbol)
#define dlclose(handle) FreeLibrary((HMODULE)(handle))
#define dlerror() "Windows DLL error"
#else
#include <dlfcn.h>
#endif

PluginManager *plugin_manager_create(void)
{
    PluginManager *manager = calloc(1, sizeof(PluginManager));
    if (!manager)
        return NULL;

    if (pthread_mutex_init(&manager->registry.mutex, NULL) != 0)
    {
        free(manager);
        return NULL;
    }

    if (pthread_mutex_init(&manager->communication.mutex, NULL) != 0)
    {
        pthread_mutex_destroy(&manager->registry.mutex);
        free(manager);
        return NULL;
    }

    for (int i = 0; i < MAX_PLUGINS; i++)
    {
        manager->communication.plugin_data_sizes[i] = 0;
    }

    manager->format_engine = NULL;
    manager->filter_engine = NULL;

    return manager;
}

// IMPORTANT: ctx might be null, because we might call it before the context is created
void plugin_manager_destroy(PluginManager *manager, FconcatContext *ctx)
{
    if (!manager)
        return;

    pthread_mutex_lock(&manager->registry.mutex);

    for (int i = 0; i < manager->registry.count; i++)
    {
        PluginMetadata *meta = &manager->registry.plugins[i];

        if (meta->type == PLUGIN_TYPE_CONTENT)
        {
            ContentPlugin *plugin = (ContentPlugin *)meta->plugin_data;
            if (plugin && plugin->cleanup)
            {
                plugin->cleanup(ctx);
            }
        }
        else if (meta->type == PLUGIN_TYPE_FORMAT)
        {
            FormatPlugin *plugin = (FormatPlugin *)meta->plugin_data;
            if (plugin && plugin->cleanup)
            {
                plugin->cleanup(ctx);
            }
        }
        else if (meta->type == PLUGIN_TYPE_FILTER)
        {
            FilterPlugin *plugin = (FilterPlugin *)meta->plugin_data;
            if (plugin && plugin->cleanup)
            {
                plugin->cleanup(ctx);
            }
        }

        if (meta->handle)
        {
            dlclose(meta->handle);
        }

        free(meta->plugin_data);
    }

    pthread_mutex_unlock(&manager->registry.mutex);
    pthread_mutex_destroy(&manager->registry.mutex);
    pthread_mutex_destroy(&manager->communication.mutex);
    free(manager);
}

int plugin_manager_configure(PluginManager *manager, const ResolvedConfig *config,
                             struct FormatEngine *format_engine,
                             struct FilterEngine *filter_engine)
{
    if (!manager || !config)
        return -1;

    manager->config = config;
    manager->format_engine = format_engine;
    manager->filter_engine = filter_engine;

    for (int i = 0; i < config->plugin_count; i++)
    {
        if (plugin_manager_load_plugin(manager, config->plugins[i].path,
                                       config->plugins[i].parameters,
                                       config->plugins[i].parameter_count) != 0)
        {
            fprintf(stderr, "Failed to load plugin: %s\n", config->plugins[i].path);
        }
    }

    return 0;
}

int plugin_manager_load_plugin(PluginManager *manager, const char *path, char **parameters, int parameter_count)
{
    if (!manager || !path)
        return -1;

    void *handle = dlopen(path, RTLD_LAZY);
    if (!handle)
    {
        fprintf(stderr, "Cannot load plugin %s: %s\n", path, dlerror());
        return -1;
    }

    // Try format plugin FIRST
    void *get_format_plugin_func = dlsym(handle, "get_format_plugin");
    if (get_format_plugin_func)
    {
        FormatPlugin *(*get_format_plugin)(void) = (FormatPlugin * (*)(void)) get_format_plugin_func;
        FormatPlugin *plugin = get_format_plugin();

        if (plugin)
        {
            pthread_mutex_lock(&manager->registry.mutex);

            if (manager->registry.count >= MAX_PLUGINS)
            {
                pthread_mutex_unlock(&manager->registry.mutex);
                dlclose(handle);
                return -1;
            }

            PluginMetadata *meta = &manager->registry.plugins[manager->registry.count];
            meta->name = plugin->name;
            meta->version = "Unknown";
            meta->description = "Format plugin";
            meta->author = "Unknown";
            meta->type = PLUGIN_TYPE_FORMAT;
            meta->initialized = false;
            meta->plugin_data = malloc(sizeof(FormatPlugin));
            if (!meta->plugin_data)
            {
                pthread_mutex_unlock(&manager->registry.mutex);
                dlclose(handle);
                return -1;
            }
            memcpy(meta->plugin_data, plugin, sizeof(FormatPlugin));
            meta->handle = handle;

            // Store plugin parameters
            meta->parameters = NULL;
            meta->parameter_count = 0;
            if (parameters && parameter_count > 0)
            {
                meta->parameters = malloc(parameter_count * sizeof(char *));
                if (meta->parameters)
                {
                    meta->parameter_count = parameter_count;
                    for (int i = 0; i < parameter_count; i++)
                    {
                        meta->parameters[i] = strdup(parameters[i] ? parameters[i] : "");
                        if (!meta->parameters[i]) {
                            for (int j = 0; j < i; j++) free(meta->parameters[j]);
                            free(meta->parameters);
                            meta->parameters = NULL;
                            meta->parameter_count = 0;
                            break;
                        }
                    }
                }
            }

            manager->registry.count++;

            // Register with format engine immediately
            if (manager->format_engine)
            {
                printf("Loading format plugin: %s (will initialize later)\n", plugin->name);
                extern int format_engine_register_plugin(struct FormatEngine * engine, FormatPlugin * plugin);
                format_engine_register_plugin(manager->format_engine, plugin);

                // AUTO-ACTIVATE the plugin
                extern int format_engine_set_active_formatter(struct FormatEngine * engine, const char *name);
                format_engine_set_active_formatter(manager->format_engine, plugin->name);
            }

            pthread_mutex_unlock(&manager->registry.mutex);
            return 0;
        }
    }

    // Try filter plugin SECOND
    void *get_filter_plugin_func = dlsym(handle, "get_filter_plugin");
    if (get_filter_plugin_func)
    {
        FilterPlugin *(*get_filter_plugin)(void) = (FilterPlugin * (*)(void)) get_filter_plugin_func;
        FilterPlugin *plugin = get_filter_plugin();

        if (plugin)
        {
            pthread_mutex_lock(&manager->registry.mutex);

            if (manager->registry.count >= MAX_PLUGINS)
            {
                pthread_mutex_unlock(&manager->registry.mutex);
                dlclose(handle);
                return -1;
            }

            PluginMetadata *meta = &manager->registry.plugins[manager->registry.count];
            meta->name = plugin->name;
            meta->version = "Unknown";
            meta->description = "Filter plugin";
            meta->author = "Unknown";
            meta->type = PLUGIN_TYPE_FILTER;
            meta->initialized = false;
            meta->plugin_data = malloc(sizeof(FilterPlugin));
            if (!meta->plugin_data)
            {
                pthread_mutex_unlock(&manager->registry.mutex);
                dlclose(handle);
                return -1;
            }
            memcpy(meta->plugin_data, plugin, sizeof(FilterPlugin));
            meta->handle = handle;

            // Store plugin parameters
            meta->parameters = NULL;
            meta->parameter_count = 0;
            if (parameters && parameter_count > 0)
            {
                meta->parameters = malloc(parameter_count * sizeof(char *));
                if (meta->parameters)
                {
                    meta->parameter_count = parameter_count;
                    for (int i = 0; i < parameter_count; i++)
                    {
                        meta->parameters[i] = strdup(parameters[i] ? parameters[i] : "");
                        if (!meta->parameters[i]) {
                            for (int j = 0; j < i; j++) free(meta->parameters[j]);
                            free(meta->parameters);
                            meta->parameters = NULL;
                            meta->parameter_count = 0;
                            break;
                        }
                    }
                }
            }

            manager->registry.count++;

            // Register with filter engine
            if (manager->filter_engine)
            {
                printf("Loading filter plugin: %s (will initialize later)\n", plugin->name);
                extern int filter_engine_register_plugin(struct FilterEngine * engine, FilterPlugin * plugin);
                filter_engine_register_plugin(manager->filter_engine, plugin);
            }

            pthread_mutex_unlock(&manager->registry.mutex);
            return 0;
        }
    }

    // Try content plugin LAST
    void *get_plugin_func = dlsym(handle, "get_plugin");
    if (get_plugin_func)
    {
        ContentPlugin *(*get_content_plugin)(void) = (ContentPlugin * (*)(void)) get_plugin_func;
        ContentPlugin *plugin = get_content_plugin();

        if (plugin)
        {
            pthread_mutex_lock(&manager->registry.mutex);

            if (manager->registry.count >= MAX_PLUGINS)
            {
                pthread_mutex_unlock(&manager->registry.mutex);
                dlclose(handle);
                return -1;
            }

            PluginMetadata *meta = &manager->registry.plugins[manager->registry.count];
            meta->name = plugin->name;
            meta->version = plugin->version;
            meta->description = plugin->description;
            meta->author = "Unknown";
            meta->type = PLUGIN_TYPE_CONTENT;
            meta->initialized = false;
            meta->plugin_data = malloc(sizeof(ContentPlugin));
            if (!meta->plugin_data)
            {
                pthread_mutex_unlock(&manager->registry.mutex);
                dlclose(handle);
                return -1;
            }
            memcpy(meta->plugin_data, plugin, sizeof(ContentPlugin));
            meta->handle = handle;

            // Store plugin parameters
            meta->parameters = NULL;
            meta->parameter_count = 0;
            if (parameters && parameter_count > 0)
            {
                meta->parameters = malloc(parameter_count * sizeof(char *));
                if (meta->parameters)
                {
                    meta->parameter_count = parameter_count;
                    for (int i = 0; i < parameter_count; i++)
                    {
                        meta->parameters[i] = strdup(parameters[i] ? parameters[i] : "");
                        if (!meta->parameters[i]) {
                            for (int j = 0; j < i; j++) free(meta->parameters[j]);
                            free(meta->parameters);
                            meta->parameters = NULL;
                            meta->parameter_count = 0;
                            break;
                        }
                    }
                }
            }

            manager->registry.count++;
            printf("Loading content plugin: %s (will initialize later)\n", plugin->name);

            pthread_mutex_unlock(&manager->registry.mutex);
            return 0;
        }
    }

    dlclose(handle);
    return -1;
}

int plugin_manager_initialize_plugins(PluginManager *manager, FconcatContext *ctx)
{
    if (!manager || !ctx)
        return -1;

    pthread_mutex_lock(&manager->registry.mutex);

    for (int i = 0; i < manager->registry.count; i++)
    {
        PluginMetadata *meta = &manager->registry.plugins[i];

        if (meta->initialized)
            continue;

        if (meta->type == PLUGIN_TYPE_FORMAT)
        {
            FormatPlugin *plugin = (FormatPlugin *)meta->plugin_data;
            if (plugin && plugin->init)
            {
                printf("Initializing format plugin: %s with %d parameters\n",
                       plugin->name, meta->parameter_count);

                pthread_mutex_unlock(&manager->registry.mutex);

                int result = plugin->init(ctx);

                pthread_mutex_lock(&manager->registry.mutex);

                if (result == 0)
                {
                    meta->initialized = true;
                }
                else
                {
                    printf("Failed to initialize format plugin: %s\n", plugin->name);
                }
            }
            else
            {
                meta->initialized = true;
            }
        }
        else if (meta->type == PLUGIN_TYPE_FILTER)
        {
            FilterPlugin *plugin = (FilterPlugin *)meta->plugin_data;
            if (plugin && plugin->init)
            {
                printf("Initializing filter plugin: %s with %d parameters\n",
                       plugin->name, meta->parameter_count);

                pthread_mutex_unlock(&manager->registry.mutex);
                int result = plugin->init(ctx);
                pthread_mutex_lock(&manager->registry.mutex);

                if (result == 0)
                {
                    meta->initialized = true;
                }
                else
                {
                    printf("Failed to initialize filter plugin: %s\n", plugin->name);
                }
            }
            else
            {
                meta->initialized = true;
            }
        }
        else if (meta->type == PLUGIN_TYPE_CONTENT)
        {
            ContentPlugin *plugin = (ContentPlugin *)meta->plugin_data;
            if (plugin && plugin->init)
            {
                printf("Initializing content plugin: %s with %d parameters\n",
                       plugin->name, meta->parameter_count);

                pthread_mutex_unlock(&manager->registry.mutex);
                int result = plugin->init(ctx);
                pthread_mutex_lock(&manager->registry.mutex);

                if (result == 0)
                {
                    meta->initialized = true;
                }
                else
                {
                    printf("Failed to initialize content plugin: %s\n", plugin->name);
                }
            }
            else
            {
                meta->initialized = true;
            }
        }
    }

    pthread_mutex_unlock(&manager->registry.mutex);
    return 0;
}

const char *plugin_manager_get_parameter(PluginManager *manager, const char *plugin_name, const char *param_name)
{
    if (!manager || !plugin_name || !param_name)
        return NULL;

    pthread_mutex_lock(&manager->registry.mutex);

    for (int i = 0; i < manager->registry.count; i++)
    {
        PluginMetadata *meta = &manager->registry.plugins[i];
        if (meta->name && strcmp(meta->name, plugin_name) == 0)
        {
            // Search for parameter by name (format: key=value or just value)
            for (int j = 0; j < meta->parameter_count; j++)
            {
                if (meta->parameters[j])
                {
                    char *equals = strchr(meta->parameters[j], '=');
                    if (equals)
                    {
                        // Key=value format
                        size_t key_len = equals - meta->parameters[j];
                        if (strncmp(meta->parameters[j], param_name, key_len) == 0 &&
                            strlen(param_name) == key_len)
                        {
                            pthread_mutex_unlock(&manager->registry.mutex);
                            return equals + 1; // Return value part
                        }
                    }
                    else if (strcmp(meta->parameters[j], param_name) == 0)
                    {
                        // Direct match
                        pthread_mutex_unlock(&manager->registry.mutex);
                        return meta->parameters[j];
                    }
                }
            }
            break;
        }
    }

    pthread_mutex_unlock(&manager->registry.mutex);
    return NULL;
}

int plugin_manager_get_parameter_count(PluginManager *manager, const char *plugin_name)
{
    if (!manager || !plugin_name)
        return 0;

    pthread_mutex_lock(&manager->registry.mutex);

    for (int i = 0; i < manager->registry.count; i++)
    {
        PluginMetadata *meta = &manager->registry.plugins[i];
        if (meta->name && strcmp(meta->name, plugin_name) == 0)
        {
            int count = meta->parameter_count;
            pthread_mutex_unlock(&manager->registry.mutex);
            return count;
        }
    }

    pthread_mutex_unlock(&manager->registry.mutex);
    return 0;
}

const char *plugin_manager_get_parameter_by_index(PluginManager *manager, const char *plugin_name, int index)
{
    if (!manager || !plugin_name || index < 0)
        return NULL;

    pthread_mutex_lock(&manager->registry.mutex);

    for (int i = 0; i < manager->registry.count; i++)
    {
        PluginMetadata *meta = &manager->registry.plugins[i];
        if (meta->name && strcmp(meta->name, plugin_name) == 0)
        {
            if (index < meta->parameter_count && meta->parameters[index])
            {
                const char *param = meta->parameters[index];
                pthread_mutex_unlock(&manager->registry.mutex);
                return param;
            }
            break;
        }
    }

    pthread_mutex_unlock(&manager->registry.mutex);
    return NULL;
}

void *plugin_manager_get_plugin_data(PluginManager *manager, const char *plugin_name)
{
    if (!manager || !plugin_name)
        return NULL;

    pthread_mutex_lock(&manager->communication.mutex);

    for (int i = 0; i < MAX_PLUGINS; i++)
    {
        if (i < manager->registry.count)
        {
            PluginMetadata *meta = &manager->registry.plugins[i];
            if (meta->name && strcmp(meta->name, plugin_name) == 0)
            {
                void *data = &manager->communication.plugin_data[i];
                pthread_mutex_unlock(&manager->communication.mutex);
                return data;
            }
        }
    }

    pthread_mutex_unlock(&manager->communication.mutex);
    return NULL;
}

int plugin_manager_set_plugin_data(PluginManager *manager, const char *plugin_name, void *data, size_t size)
{
    if (!manager || !plugin_name || !data)
        return -1;

    if (size > PLUGIN_DATA_SIZE)
    {
        fprintf(stderr, "Plugin data size %zu exceeds maximum %d\n", size, PLUGIN_DATA_SIZE);
        return -1;
    }

    pthread_mutex_lock(&manager->communication.mutex);

    for (int i = 0; i < MAX_PLUGINS; i++)
    {
        if (i < manager->registry.count)
        {
            PluginMetadata *meta = &manager->registry.plugins[i];
            if (meta->name && strcmp(meta->name, plugin_name) == 0)
            {
                memcpy(&manager->communication.plugin_data[i], data, size);
                manager->communication.plugin_data_sizes[i] = size;
                pthread_mutex_unlock(&manager->communication.mutex);
                return 0;
            }
        }
    }

    pthread_mutex_unlock(&manager->communication.mutex);
    return -1;
}

int plugin_manager_call_plugin_method(PluginManager *manager, const char *plugin_name, const char *method, void *args)
{
    (void)manager;
    (void)plugin_name;
    (void)method;
    (void)args;
    if (!manager || !plugin_name || !method)
        return -1;

    return -1;
}