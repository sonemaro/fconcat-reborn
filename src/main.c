#include "fconcat.h"
#include "core/context.h"
#include "plugins/plugin.h"
#include "format/format.h"
#include "filter/filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

// Global managers for signal handling
static PluginManager *g_plugin_manager = NULL;
static ErrorManager *g_error_manager = NULL;
static MemoryManager *g_memory_manager = NULL;

// Signal handler for graceful shutdown
static void signal_handler(int signum)
{
    printf("\n🔌 Received signal %d, shutting down...\n", signum);

    if (g_plugin_manager)
    {
        plugin_manager_destroy(g_plugin_manager, NULL);
        g_plugin_manager = NULL;
    }

    if (g_error_manager)
    {
        error_manager_destroy(g_error_manager);
        g_error_manager = NULL;
    }

    if (g_memory_manager)
    {
        memory_manager_destroy(g_memory_manager);
        g_memory_manager = NULL;
    }

    exit(EXIT_SUCCESS);
}

void print_header()
{
    printf("fconcat v%s - Next-generation file concatenator\n", FCONCAT_VERSION);
    printf("%s\n", FCONCAT_COPYRIGHT);
    printf("==================================================================\n\n");
}

void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s <input_directory> <output_file> [options]\n"
            "\n"
            "Options:\n"
            "  <input_directory>     Path to the directory to scan and concatenate.\n"
            "  <output_file>         Path to the output file to write results.\n"
            "  --exclude <patterns>  Exclude files/directories matching patterns.\n"
            "                        Supports wildcards (* and ?) and multiple patterns.\n"
            "                        Example: --exclude \"*.log\" \"build/*\" \"temp?.txt\"\n"
            "  --show-size, -s       Display file sizes in the directory structure.\n"
            "  --verbose, -v         Enable verbose logging (sets log level to debug).\n"
            "  --log-level <level>   Set log level: error, warning, info, debug, trace\n"
            "  --interactive         Keep plugins active after processing.\n"
            "  --binary-skip         Skip binary files entirely (default).\n"
            "  --binary-include      Include binary files in concatenation.\n"
            "  --binary-placeholder  Show placeholder for binary files.\n"
            "  --symlinks <mode>     How to handle symbolic links:\n"
            "                        skip, follow, include, placeholder\n"
            "  --format <format>     Output format: text, json\n"
            "  --plugin <spec>       Load a plugin with optional parameters.\n"
            "                        Format: path[:param1=value1,param2=value2,...]\n"
            "\n"
            "Examples:\n"
            "  %s ./src all.txt\n"
            "  %s ./project result.json --format json --show-size\n"
            "  %s ./code output.txt --exclude \"*.log\" \"node_modules/*\" \"*.tmp\"\n"
            "  %s ./kernel out.txt --exclude \"*.o\" \"*.ko\" --binary-skip\n"
            "  %s ./project out.txt --log-level debug --plugin ./plugin.so:debug=1,format=json\n"
            "\n"
            "Plugin parameters:\n"
            "  Plugins can accept parameters in key=value format, separated by commas\n"
            "  Example: --plugin ./myplugin.so:debug=1,threshold=100,mode=strict\n"
            "\n"
            "Exclude patterns support:\n"
            "  * matches any sequence of characters\n"
            "  ? matches any single character\n"
            "  Patterns match both full paths and basenames\n"
            "  Multiple patterns can be specified\n"
            "\n"
            "Log levels (from lowest to highest verbosity):\n"
            "  error    - Only error messages\n"
            "  warning  - Warnings and errors\n"
            "  info     - Information, warnings, and errors (default)\n"
            "  debug    - Debug info and above\n"
            "  trace    - All messages including trace\n"
            "\n"
            "For more information, visit: https://github.com/sonemaro/fconcat\n",
            program_name, program_name, program_name, program_name, program_name, program_name);
}

int main(int argc, char *argv[])
{
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    print_header();

    if (argc < 3)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize core managers
    g_error_manager = error_manager_create();
    if (!g_error_manager)
    {
        fprintf(stderr, "Failed to initialize error manager\n");
        return EXIT_FAILURE;
    }

    g_memory_manager = memory_manager_create();
    if (!g_memory_manager)
    {
        fprintf(stderr, "Failed to initialize memory manager\n");
        error_manager_destroy(g_error_manager);
        return EXIT_FAILURE;
    }

    g_plugin_manager = plugin_manager_create();
    if (!g_plugin_manager)
    {
        fprintf(stderr, "Failed to initialize plugin manager\n");
        memory_manager_destroy(g_memory_manager);
        error_manager_destroy(g_error_manager);
        return EXIT_FAILURE;
    }

    // Initialize main variables
    ConfigManager *config_manager = NULL;
    FormatEngine *format_engine = NULL;
    FilterEngine *filter_engine = NULL;
    FILE *output_file = NULL;
    FconcatContext *ctx = NULL;
    int result = -1;

    // Initialize configuration system
    config_manager = config_manager_create();
    if (!config_manager)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_OUT_OF_MEMORY, "Failed to create configuration manager");
        goto cleanup;
    }

    // Load configuration
    if (config_load_defaults(config_manager) != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_CONFIG_INVALID, "Failed to load default configuration");
        goto cleanup;
    }

    if (config_load_cli(config_manager, argc, argv) != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_CONFIG_INVALID, "Failed to load CLI configuration");
        goto cleanup;
    }

    ResolvedConfig *config = config_resolve(config_manager);
    if (!config)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_CONFIG_INVALID, "Failed to resolve configuration");
        goto cleanup;
    }

    // Initialize engines
    format_engine = format_engine_create();
    if (!format_engine)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_OUT_OF_MEMORY, "Failed to create format engine");
        goto cleanup;
    }

    filter_engine = filter_engine_create();
    if (!filter_engine)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_OUT_OF_MEMORY, "Failed to create filter engine");
        goto cleanup;
    }

    // Open output file
    output_file = fopen(config->output_file, "wb");
    if (!output_file)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_FILE_NOT_FOUND, "Cannot open output file: %s", config->output_file);
        goto cleanup;
    }

    // Configure engines
    if (format_engine_configure(format_engine, config, output_file) != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_CONFIG_INVALID, "Failed to configure format engine");
        goto cleanup;
    }

    if (filter_engine_configure(filter_engine, config) != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_CONFIG_INVALID, "Failed to configure filter engine");
        goto cleanup;
    }

    if (plugin_manager_configure(g_plugin_manager, config, format_engine, filter_engine) != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_CONFIG_INVALID, "Failed to configure plugin manager");
        goto cleanup;
    }

    // Initialize processing statistics
    ProcessingStats stats = {0};
    stats.start_time = (double)start_time.tv_sec + start_time.tv_nsec / 1000000000.0;

    // Create fconcat context
    ctx = create_fconcat_context(
        config, output_file, &stats,
        g_error_manager, g_memory_manager, g_plugin_manager,
        format_engine, filter_engine);

    if (!ctx)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_OUT_OF_MEMORY, "Failed to create processing context");
        goto cleanup;
    }

    printf("DEBUG: About to initialize plugins with context\n");
    fflush(stdout);
    plugin_manager_initialize_plugins(g_plugin_manager, ctx);
    printf("DEBUG: Plugin initialization completed\n");
    fflush(stdout);

    plugin_manager_initialize_plugins(g_plugin_manager, ctx);

    ctx->log(ctx, LOG_DEBUG, "Processing directory: %s\n", config->input_directory);
    ctx->log(ctx, LOG_DEBUG, "Output file: %s\n", config->output_file);
    ctx->log(ctx, LOG_DEBUG, "Format: %s\n", config->output_format);
    ctx->log(ctx, LOG_DEBUG, "Plugins: %d loaded\n", g_plugin_manager->registry.count);

    // Begin processing
    ctx->log(ctx, LOG_DEBUG, "Beginning processing");
    result = 0;

    // Start document
    ctx->log(ctx, LOG_DEBUG, "Starting document");
    result = format_engine_begin_document(format_engine, ctx);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to begin document");
        goto cleanup;
    }

    // Process directory structure
    ctx->log(ctx, LOG_DEBUG, "Beginning structure processing");
    result = format_engine_begin_structure(format_engine, ctx);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to begin structure");
        goto cleanup;
    }

    ctx->log(ctx, LOG_DEBUG, "Processing directory structure");
    result = process_directory_structure(ctx, config->input_directory, "", 0);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to process directory structure");
        goto cleanup;
    }

    result = format_engine_end_structure(format_engine, ctx);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to end structure");
        goto cleanup;
    }

    // Process file contents
    ctx->log(ctx, LOG_DEBUG, "Beginning content processing");
    result = format_engine_begin_content(format_engine, ctx);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to begin content");
        goto cleanup;
    }

    ctx->log(ctx, LOG_DEBUG, "Processing file contents");
    result = process_directory_content(ctx, config->input_directory, "", 0);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to process directory content");
        goto cleanup;
    }

    result = format_engine_end_content(format_engine, ctx);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to end content");
        goto cleanup;
    }

    // End document
    ctx->log(ctx, LOG_DEBUG, "Ending document");
    result = format_engine_end_document(format_engine, ctx);
    if (result != 0)
    {
        ERROR_REPORT(g_error_manager, FCONCAT_ERROR_IO_ERROR, "Failed to end document");
        goto cleanup;
    }

    // Calculate processing time
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

    printf("✅ Processing completed successfully!\n");
    printf("⏱️  Processing time: %.3f seconds\n", elapsed);
    printf("📊 Files processed: %zu\n", stats.processed_files);
    printf("📈 Bytes processed: %zu\n", stats.processed_bytes);

    // Memory statistics
    MemoryStats memory_stats = memory_get_stats(g_memory_manager);
    printf("🧠 Memory usage: %zu bytes peak\n", memory_stats.peak_usage);

    // Interactive mode
    if (config->interactive)
    {
        printf("\n🔌 Entering interactive mode...\n");
        printf("Press Enter to exit, or Ctrl+C to force quit\n");

        char buffer[256];
        if (fgets(buffer, sizeof(buffer), stdin))
        {
            // Input received
        }

        printf("🔌 Shutting down plugins...\n");
    }

    printf("Thank you for using fconcat! 🚀\n");

cleanup:
    // Cleanup resources
    // IMPORTANT: WE NEED TO CLEANUP PLUGIN MANAGER FIRST OR WE WILL NOT BE
    // ABLE TO USE CTX IN PLUGIN DESTROY FUNCTION
    if (g_plugin_manager)
    {
        plugin_manager_destroy(g_plugin_manager, ctx);
        g_plugin_manager = NULL;
    }
    if (ctx)
    {
        destroy_fconcat_context(ctx);
    }

    if (output_file)
    {
        fclose(output_file);
    }

    if (format_engine)
    {
        format_engine_destroy(format_engine);
    }

    if (filter_engine)
    {
        filter_engine_destroy(filter_engine);
    }

    if (config_manager)
    {
        config_manager_destroy(config_manager);
    }
    if (g_memory_manager)
    {
        memory_manager_destroy(g_memory_manager);
        g_memory_manager = NULL;
    }

    if (g_error_manager)
    {
        int error_count = error_get_count(g_error_manager);
        if (error_count > 0)
        {
            printf("❌ %d errors encountered during processing\n", error_count);
        }
        error_manager_destroy(g_error_manager);
        g_error_manager = NULL;
    }

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}