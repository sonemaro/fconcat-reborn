#ifndef PLUGINS_PLUGIN_H
#define PLUGINS_PLUGIN_H

#include "../fconcat.h"
#include "../../include/fconcat_content.h"
#include "../../include/fconcat_format.h"
#include "../../include/fconcat_filter.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLUGIN_DATA_SIZE 1024

    typedef struct
    {
        const char *name;
        const char *version;
        const char *description;
        const char *author;
        PluginType type;
        void *plugin_data;
        void *handle;
        char **parameters;
        int parameter_count;
        bool initialized;
    } PluginMetadata;

    typedef struct
    {
        PluginMetadata plugins[MAX_PLUGINS];
        int count;
        pthread_mutex_t mutex;
    } PluginRegistry;

    typedef struct
    {
        char plugin_data[MAX_PLUGINS][PLUGIN_DATA_SIZE];
        size_t plugin_data_sizes[MAX_PLUGINS];
        pthread_mutex_t mutex;
    } PluginCommunication;

    typedef struct PluginManager
    {
        PluginRegistry registry;
        PluginCommunication communication;
        const ResolvedConfig *config;
        struct FormatEngine *format_engine;
        struct FilterEngine *filter_engine;
    } PluginManager;

    // Plugin functions
    PluginManager *plugin_manager_create(void);
    void plugin_manager_destroy(PluginManager *manager, FconcatContext *ctx);
    int plugin_manager_configure(PluginManager *manager, const ResolvedConfig *config,
                                 struct FormatEngine *format_engine,
                                 struct FilterEngine *filter_engine);
    int plugin_manager_load_plugin(PluginManager *manager, const char *path, char **parameters, int parameter_count);
    int plugin_manager_initialize_plugins(PluginManager *manager, FconcatContext *ctx);
    void *plugin_manager_get_plugin_data(PluginManager *manager, const char *plugin_name);
    int plugin_manager_set_plugin_data(PluginManager *manager, const char *plugin_name, void *data, size_t size);
    int plugin_manager_call_plugin_method(PluginManager *manager, const char *plugin_name, const char *method, void *args);

    // Plugin parameter access
    const char *plugin_manager_get_parameter(PluginManager *manager, const char *plugin_name, const char *param_name);
    int plugin_manager_get_parameter_count(PluginManager *manager, const char *plugin_name);
    const char *plugin_manager_get_parameter_by_index(PluginManager *manager, const char *plugin_name, int index);

#ifdef __cplusplus
}
#endif

#endif /* PLUGINS_PLUGIN_H */