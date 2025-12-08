// File: include/fconcat_api.h
#ifndef FCONCAT_API_H
#define FCONCAT_API_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Plugin export macro
#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

    // Forward declarations - minimal set
    typedef struct FconcatContext FconcatContext;
    typedef struct PluginFileContext PluginFileContext;

    // Log levels
    typedef enum
    {
        LOG_ERROR = 0,
        LOG_WARNING = 1,
        LOG_INFO = 2,
        LOG_DEBUG = 3,
        LOG_TRACE = 4
    } LogLevel;

    // Progress callback
    typedef void (*ProgressCallback)(const char *operation, size_t current, size_t total, void *user_data);

    struct FconcatContext
    {
        // Configuration access
        const void *config; // Opaque pointer
        const char *(*get_config_string)(FconcatContext *ctx, const char *key);
        int (*get_config_int)(FconcatContext *ctx, const char *key);
        bool (*get_config_bool)(FconcatContext *ctx, const char *key);

        // Plugin parameter access
        const char *(*get_plugin_parameter)(FconcatContext *ctx, const char *plugin_name, const char *param_name);
        int (*get_plugin_parameter_count)(FconcatContext *ctx, const char *plugin_name);
        const char *(*get_plugin_parameter_by_index)(FconcatContext *ctx, const char *plugin_name, int index);

        // Logging system
        void (*log)(FconcatContext *ctx, LogLevel level, const char *format, ...);
        void (*vlog)(FconcatContext *ctx, LogLevel level, const char *format, va_list args);
        bool (*is_log_enabled)(FconcatContext *ctx, LogLevel level);

        // Memory management
        void *(*alloc)(FconcatContext *ctx, size_t size);
        void *(*realloc)(FconcatContext *ctx, void *ptr, size_t size);
        void (*free)(FconcatContext *ctx, void *ptr);

        // Output writing
        int (*write_output)(FconcatContext *ctx, const char *data, size_t size);
        int (*write_output_fmt)(FconcatContext *ctx, const char *format, ...);

        // Error reporting
        void (*error)(FconcatContext *ctx, const char *format, ...);
        void (*warning)(FconcatContext *ctx, const char *format, ...);
        int (*get_error_count)(FconcatContext *ctx);

        /**
         * Current processing state
         * 
         * WARNING: These pointers are only valid during callback execution.
         * - current_file_path points to a stack-allocated buffer
         * - current_file_info points to a stack-allocated FileInfo struct
         * 
         * Do NOT store these pointers for later use. If you need the values
         * after the callback returns, make a copy using strdup() or memcpy().
         */
        const char *current_file_path;
        const void *current_file_info; // Opaque pointer to FileInfo, valid only during callback
        size_t current_file_processed_bytes;
        int current_directory_level;

        // Processing statistics
        void *stats; // Opaque pointer

        // Progress reporting
        void (*progress)(FconcatContext *ctx, const char *operation, size_t current, size_t total);
        void (*set_progress_callback)(FconcatContext *ctx, ProgressCallback callback, void *user_data);

        // Plugin functions
        void *(*get_plugin_data)(FconcatContext *ctx, const char *plugin_name);
        int (*set_plugin_data)(FconcatContext *ctx, const char *plugin_name, void *data, size_t size);
        int (*call_plugin_method)(FconcatContext *ctx, const char *plugin_name, const char *method, void *args);

        // Stream utilities
        void *(*create_stream_buffer)(FconcatContext *ctx, size_t initial_size);
        int (*stream_write)(FconcatContext *ctx, void *buffer, const char *data, size_t size);
        int (*stream_flush)(FconcatContext *ctx, void *buffer);
        void (*stream_destroy)(FconcatContext *ctx, void *buffer);

        // File system services
        bool (*file_exists)(FconcatContext *ctx, const char *path);
        int (*get_file_info)(FconcatContext *ctx, const char *path, void *info);
        char *(*resolve_path)(FconcatContext *ctx, const char *relative_path);

        // System information
        const char *fconcat_version;
        const char *build_info;
        time_t processing_start_time;
        const char *output_file_path;

        // Internal state (opaque)
        void *internal_state;
    };

#ifdef __cplusplus
}
#endif

#endif /* FCONCAT_API_H */