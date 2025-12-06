#ifndef FILTER_FILTER_H
#define FILTER_FILTER_H

#include "../core/types.h"
#include "../core/context.h"
#include "../../include/fconcat_filter.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Forward declaration
    struct FconcatContext;

    // Filter rule
    typedef struct
    {
        FilterType type;
        int priority;
        int (*match_path)(const char *path, FileInfo *info, void *context);
        int (*match_content)(const char *path, const char *content, size_t size, void *context);
        int (*transform)(const char *path, const char *input, size_t input_size, char **output, size_t *output_size, void *context);
        void (*destroy_context)(void *context); 
        void *context;
    } FilterRule;

    // Filter engine
    typedef struct FilterEngine
    {
        FilterRule *rules;
        int rule_count;
        int rule_capacity;
        FilterPlugin *plugins[MAX_PLUGINS];
        int plugin_count;
        const ResolvedConfig *config;
        pthread_mutex_t mutex;
    } FilterEngine;

    // Exclude pattern context (shared between filter modules)
    typedef struct
    {
        char **patterns;
        int pattern_count;
    } ExcludeContext;

    // Include pattern context
    typedef struct
    {
        char **patterns;
        int pattern_count;
    } IncludeContext;

    // Filter functions
    FilterEngine *filter_engine_create(void);
    void filter_engine_destroy(FilterEngine *engine);
    int filter_engine_configure(FilterEngine *engine, const ResolvedConfig *config);
    int filter_engine_register_plugin(FilterEngine *engine, FilterPlugin *plugin);
    int filter_engine_add_rule(FilterEngine *engine, FilterRule *rule);
    int filter_engine_should_include_path(FilterEngine *engine, struct FconcatContext *ctx, const char *path, FileInfo *info);
    int filter_engine_should_include_content(FilterEngine *engine, struct FconcatContext *ctx, const char *path, const char *content, size_t size);
    int filter_engine_transform_content(FilterEngine *engine, struct FconcatContext *ctx, const char *path, const char *input, size_t input_size, char **output, size_t *output_size);

    // Built-in filters
    int filter_exclude_patterns_init(FilterEngine *engine, const ResolvedConfig *config);
    int filter_include_patterns_init(FilterEngine *engine, const ResolvedConfig *config); 
    int filter_binary_detection_init(FilterEngine *engine, const ResolvedConfig *config);
    int filter_symlink_handling_init(FilterEngine *engine, const ResolvedConfig *config);

    int filter_engine_add_rule_internal(FilterEngine *engine, const FilterRule *rule);
    int filter_exclude_patterns_init_internal(FilterEngine *engine, const ResolvedConfig *config);
    int filter_include_patterns_init_internal(FilterEngine *engine, const ResolvedConfig *config); 
    int filter_binary_detection_init_internal(FilterEngine *engine, const ResolvedConfig *config);
    int filter_symlink_handling_init_internal(FilterEngine *engine, const ResolvedConfig *config);

    int exclude_match_path(const char *path, FileInfo *info, void *context);
    int include_match_path(const char *path, FileInfo *info, void *context); 
    void destroy_exclude_context_wrapper(void *context);
    void destroy_include_context_wrapper(void *context); 
    char *get_absolute_path_util(const char *path);
    char *get_relative_path_util(const char *base_dir, const char *target_path);
    const char *get_filename_util(const char *path);
    int filter_is_binary_file(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* FILTER_FILTER_H */