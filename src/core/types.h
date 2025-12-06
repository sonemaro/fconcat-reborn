#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Configuration constants
#define MAX_CONFIG_LAYERS 8
#define MAX_PLUGINS 32
#define MAX_EXCLUDES 1000
#define MAX_INCLUDES 1000
#define MAX_BUFFER_SIZE 1024 * 4
#define BINARY_CHECK_SIZE 8192
#define PLUGIN_CHUNK_SIZE 4096
#define MAX_PATH 4096
#define MAX_PLUGIN_PARAMS 16

#define MAX_FILE_SIZE (1024ULL * 1024 * 1024)       // 1GB max file size
#define MAX_STREAM_BUFFER_SIZE (256ULL * 1024 * 1024) // 256MB max buffer
#define MAX_DIRECTORY_DEPTH 256                      // Max recursion depth
#define MAX_TOTAL_FILES 1000000                      // Max files to process

    // Processing statistics
    typedef struct
    {
        size_t total_files;
        size_t processed_files;
        size_t skipped_files;
        size_t total_bytes;
        size_t processed_bytes;
        size_t filtered_bytes;
        double start_time;
        double current_time;
    } ProcessingStats;

    // File information
    typedef struct
    {
        char *path;
        size_t size;
        time_t modified_time;
        bool is_directory;
        bool is_symlink;
        bool is_binary;
        uint32_t permissions;
    } FileInfo;

    // Binary handling modes
    typedef enum
    {
        BINARY_SKIP,
        BINARY_INCLUDE,
        BINARY_PLACEHOLDER
    } BinaryHandling;

    // Symlink handling modes
    typedef enum
    {
        SYMLINK_SKIP,
        SYMLINK_FOLLOW,
        SYMLINK_INCLUDE,
        SYMLINK_PLACEHOLDER
    } SymlinkHandling;

    // Configuration source types
    typedef enum
    {
        CONFIG_SOURCE_DEFAULTS,
        CONFIG_SOURCE_FILE,
        CONFIG_SOURCE_ENV,
        CONFIG_SOURCE_CLI
    } ConfigSource;

    // Configuration value types
    typedef enum
    {
        CONFIG_TYPE_STRING,
        CONFIG_TYPE_INT,
        CONFIG_TYPE_BOOL,
        CONFIG_TYPE_FLOAT
    } ConfigType;

    // Configuration value
    typedef struct
    {
        char *key;
        ConfigType type;
        union
        {
            char *string_value;
            int int_value;
            bool bool_value;
            double float_value;
        } value;
    } ConfigValue;

    // Plugin configuration
    typedef struct
    {
        char *path;
        char **parameters;
        int parameter_count;
    } PluginConfig;

    // Resolved configuration
    typedef struct
    {
        BinaryHandling binary_handling;
        SymlinkHandling symlink_handling;
        bool show_size;
        bool verbose;
        bool interactive;
        int log_level;
        char *output_format;
        char *input_directory;
        char *output_file;
        char **exclude_patterns;
        int exclude_count;
        char **include_patterns;  
        int include_count;        
        PluginConfig *plugins;
        int plugin_count;
    } ResolvedConfig;

    // Plugin types
    typedef enum
    {
        PLUGIN_TYPE_CONTENT,
        PLUGIN_TYPE_FORMAT,
        PLUGIN_TYPE_FILTER
    } PluginType;

    // Error codes
    typedef enum
    {
        FCONCAT_SUCCESS = 0,
        FCONCAT_ERROR_INVALID_ARGS = 1,
        FCONCAT_ERROR_FILE_NOT_FOUND = 2,
        FCONCAT_ERROR_PERMISSION_DENIED = 3,
        FCONCAT_ERROR_OUT_OF_MEMORY = 4,
        FCONCAT_ERROR_PLUGIN_LOAD_FAILED = 5,
        FCONCAT_ERROR_CONFIG_INVALID = 6,
        FCONCAT_ERROR_IO_ERROR = 7
    } FconcatErrorCode;

#ifdef __cplusplus
}
#endif

#endif /* CORE_TYPES_H */