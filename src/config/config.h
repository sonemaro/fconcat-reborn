#ifndef CONFIG_CONFIG_H
#define CONFIG_CONFIG_H

#include "../core/types.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Configuration layer
    typedef struct
    {
        ConfigSource source;
        int priority;
        ConfigValue *values;
        int value_count;
        int value_capacity;
        void *source_data;
    } ConfigLayer;

    // Configuration manager
    typedef struct
    {
        ConfigLayer layers[MAX_CONFIG_LAYERS];
        int layer_count;
        ResolvedConfig *resolved;
        pthread_mutex_t mutex;
    } ConfigManager;

    // Configuration functions
    ConfigManager *config_manager_create(void);
    void config_manager_destroy(ConfigManager *manager);
    int config_load_defaults(ConfigManager *manager);
    int config_load_file(ConfigManager *manager, const char *path);
    int config_load_environment(ConfigManager *manager);
    int config_load_cli(ConfigManager *manager, int argc, char *argv[]);
    ResolvedConfig *config_resolve(ConfigManager *manager);
    const char *config_get_string(ConfigManager *manager, const char *key);
    int config_get_int(ConfigManager *manager, const char *key);
    bool config_get_bool(ConfigManager *manager, const char *key);
    double config_get_float(ConfigManager *manager, const char *key);

    // Configuration value functions
    int config_value_init(ConfigValue *value, const char *key, ConfigType type);
    void config_value_cleanup(ConfigValue *value);
    void config_value_set_string(ConfigValue *value, const char *str);
    void config_value_set_int(ConfigValue *value, int val);
    void config_value_set_bool(ConfigValue *value, bool val);
    void config_value_set_float(ConfigValue *value, double val);

    // Layer management
    int config_layer_add_value(ConfigLayer *layer, const char *key, ConfigType type);
    ConfigValue *config_layer_get_value(ConfigLayer *layer, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_CONFIG_H */