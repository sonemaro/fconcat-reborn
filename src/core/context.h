#ifndef CORE_CONTEXT_H
#define CORE_CONTEXT_H

#include "../core/types.h"
#include "../core/error.h"
#include "../core/memory.h"
#include "../../include/fconcat_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Forward declarations
    struct PluginManager;
    struct FormatEngine;
    struct FilterEngine;

    // Directory entry callback type
    typedef enum
    {
        ENTRY_TYPE_DIRECTORY,
        ENTRY_TYPE_FILE
    } EntryType;

    typedef struct
    {
        int (*handle_entry)(FconcatContext *ctx, const char *path, EntryType type,
                            FileInfo *info, int level, void *user_data);
        void *user_data;
    } DirectoryCallback;

    // Internal context state
    typedef struct
    {
        FILE *output_file;
        const ResolvedConfig *config;
        ProcessingStats *stats;
        ErrorManager *error_manager;
        MemoryManager *memory_manager;
        struct PluginManager *plugin_manager;
        struct FormatEngine *format_engine;
        struct FilterEngine *filter_engine;
        ProgressCallback progress_callback;
        void *progress_user_data;
    } InternalContextState;

    // Context creation and management
    FconcatContext *create_fconcat_context(const ResolvedConfig *config,
                                           FILE *output_file,
                                           ProcessingStats *stats,
                                           ErrorManager *error_manager,
                                           MemoryManager *memory_manager,
                                           struct PluginManager *plugin_manager,
                                           struct FormatEngine *format_engine,
                                           struct FilterEngine *filter_engine);

    void destroy_fconcat_context(FconcatContext *ctx);
    void update_context_for_file(FconcatContext *ctx, const char *filepath, const FileInfo *info);
    void update_context_progress(FconcatContext *ctx, size_t bytes_processed);

    int traverse_directory(FconcatContext *ctx, const char *base_path, const char *relative_path,
                           int level, DirectoryCallback *callback);
    int process_directory_structure(FconcatContext *ctx, const char *base_path, const char *relative_path, int level);
    int process_directory_content(FconcatContext *ctx, const char *base_path, const char *relative_path, int level);

    // Context service implementations (now take FconcatContext* as first parameter)
    const char *context_get_config_string(FconcatContext *ctx, const char *key);
    int context_get_config_int(FconcatContext *ctx, const char *key);
    bool context_get_config_bool(FconcatContext *ctx, const char *key);

    // Plugin parameter access functions
    const char *context_get_plugin_parameter(FconcatContext *ctx, const char *plugin_name, const char *param_name);
    int context_get_plugin_parameter_count(FconcatContext *ctx, const char *plugin_name);
    const char *context_get_plugin_parameter_by_index(FconcatContext *ctx, const char *plugin_name, int index);

    void context_log(FconcatContext *ctx, LogLevel level, const char *format, ...);
    void context_vlog(FconcatContext *ctx, LogLevel level, const char *format, va_list args);
    bool context_is_log_enabled(FconcatContext *ctx, LogLevel level);
    void *context_alloc(FconcatContext *ctx, size_t size);
    void *context_realloc(FconcatContext *ctx, void *ptr, size_t size);
    void context_free(FconcatContext *ctx, void *ptr);
    int context_write_output(FconcatContext *ctx, const char *data, size_t size);
    int context_write_output_fmt(FconcatContext *ctx, const char *format, ...);
    void context_error(FconcatContext *ctx, const char *format, ...);
    void context_warning(FconcatContext *ctx, const char *format, ...);
    int context_get_error_count(FconcatContext *ctx);
    void context_progress(FconcatContext *ctx, const char *operation, size_t current, size_t total);
    void context_set_progress_callback(FconcatContext *ctx, ProgressCallback callback, void *user_data);
    void *context_get_plugin_data(FconcatContext *ctx, const char *plugin_name);
    int context_set_plugin_data(FconcatContext *ctx, const char *plugin_name, void *data, size_t size);
    int context_call_plugin_method(FconcatContext *ctx, const char *plugin_name, const char *method, void *args);
    void *context_create_stream_buffer(FconcatContext *ctx, size_t initial_size);
    int context_stream_write(FconcatContext *ctx, void *buffer, const char *data, size_t size);
    int context_stream_flush(FconcatContext *ctx, void *buffer);
    void context_stream_destroy(FconcatContext *ctx, void *buffer);
    bool context_file_exists(FconcatContext *ctx, const char *path);
    int context_get_file_info(FconcatContext *ctx, const char *path, void *info);
    char *context_resolve_path(FconcatContext *ctx, const char *relative_path);

#ifdef __cplusplus
}
#endif

#endif /* CORE_CONTEXT_H */