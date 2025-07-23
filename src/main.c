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
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

// Global managers for signal handling
static PluginManager *g_plugin_manager = NULL;
static ErrorManager *g_error_manager = NULL;
static MemoryManager *g_memory_manager = NULL;
static FconcatContext *g_context = NULL;
static volatile sig_atomic_t shutdown_requested = 0;
static volatile sig_atomic_t force_shutdown = 0;
static pid_t main_pid;

// Nuclear option - force kill everything
static void nuclear_shutdown(void)
{
    printf("💥 NUCLEAR SHUTDOWN - TERMINATING EVERYTHING!\n");
    fflush(stdout);

    // Kill entire process group
    kill(0, SIGKILL);

    // If that didn't work, kill just this process
    kill(getpid(), SIGKILL);

    // Last resort - abort
    abort();
}

// Signal handler for graceful shutdown
static void signal_handler(int signum)
{
    // Increment shutdown counter for force quit detection
    shutdown_requested++;

    printf("\n🔌 Received signal %d, shutting down", signum);
    if (shutdown_requested > 1 && shutdown_requested <= 3)
    {
        printf(" (press %d more times to force quit)", 3 - shutdown_requested);
    }
    printf("...\n");
    fflush(stdout);

    // IMMEDIATE nuclear shutdown after 2 Ctrl+C presses
    if (shutdown_requested >= 2)
    {
        printf("💥 IMMEDIATE FORCE SHUTDOWN!\n");
        fflush(stdout);
        nuclear_shutdown();
        return; // Never reached
    }

    // First shutdown attempt - try graceful cleanup with timeout
    if (shutdown_requested == 1)
    {
        printf("🔄 Attempting graceful shutdown...\n");
        fflush(stdout);

        // Reset signal handler for immediate next response
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // Start a separate thread or process for timeout instead of alarm
        // to avoid conflicts with other alarms

        // Try to cleanup plugins with context if available
        if (g_plugin_manager && g_context)
        {
            printf("🔌 Shutting down plugins...\n");
            fflush(stdout);
            plugin_manager_destroy(g_plugin_manager, g_context);
            g_plugin_manager = NULL;
        }
        else if (g_plugin_manager)
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

        printf("✅ Graceful shutdown completed\n");
        fflush(stdout);
        exit(EXIT_SUCCESS);
    }
}

// Setup comprehensive signal handling
static void setup_signal_handling(void)
{
    struct sigaction sa;

    // Store main PID for process group operations
    main_pid = getpid();

    // Setup main signal handler
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // Restart interrupted system calls

    // Handle various termination signals
    sigaction(SIGINT, &sa, NULL);  // Ctrl+C
    sigaction(SIGTERM, &sa, NULL); // Termination request
    sigaction(SIGQUIT, &sa, NULL);

    // Ignore SIGPIPE to prevent crashes on broken pipes
    signal(SIGPIPE, SIG_IGN);

    // Don't set up SIGALRM handler to avoid conflicts
}

// Check if shutdown was requested
static int is_shutdown_requested(void)
{
    return shutdown_requested > 0 || force_shutdown;
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
            "Signal Handling:\n"
            "  Ctrl+C (SIGINT)       - First press: graceful shutdown, Second press: IMMEDIATE KILL\n"
            "  Ctrl+\\ (SIGQUIT)      - Immediate shutdown\n"
            "  SIGTERM               - Termination request\n"
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

// Safe processing with shutdown checks
static int safe_process_with_shutdown_check(FconcatContext *ctx, const ResolvedConfig *config)
{
    int result = 0;

    // Check shutdown before each major operation
    if (is_shutdown_requested())
    {
        printf("🛑 Shutdown requested before processing\n");
        return -1;
    }

    // Start document
    ctx->log(ctx, LOG_DEBUG, "Starting document");
    result = format_engine_begin_document(((InternalContextState *)ctx->internal_state)->format_engine, ctx);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during document start\n");
        return result != 0 ? result : -1;
    }

    // Process directory structure
    ctx->log(ctx, LOG_DEBUG, "Beginning structure processing");
    result = format_engine_begin_structure(((InternalContextState *)ctx->internal_state)->format_engine, ctx);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during structure begin\n");
        return result != 0 ? result : -1;
    }

    ctx->log(ctx, LOG_DEBUG, "Processing directory structure");
    result = process_directory_structure(ctx, config->input_directory, "", 0);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during structure processing\n");
        return result != 0 ? result : -1;
    }

    result = format_engine_end_structure(((InternalContextState *)ctx->internal_state)->format_engine, ctx);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during structure end\n");
        return result != 0 ? result : -1;
    }

    // Process file contents
    ctx->log(ctx, LOG_DEBUG, "Beginning content processing");
    result = format_engine_begin_content(((InternalContextState *)ctx->internal_state)->format_engine, ctx);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during content begin\n");
        return result != 0 ? result : -1;
    }

    ctx->log(ctx, LOG_DEBUG, "Processing file contents");
    result = process_directory_content(ctx, config->input_directory, "", 0);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during content processing\n");
        return result != 0 ? result : -1;
    }

    result = format_engine_end_content(((InternalContextState *)ctx->internal_state)->format_engine, ctx);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during content end\n");
        return result != 0 ? result : -1;
    }

    // End document
    ctx->log(ctx, LOG_DEBUG, "Ending document");
    result = format_engine_end_document(((InternalContextState *)ctx->internal_state)->format_engine, ctx);
    if (result != 0 || is_shutdown_requested())
    {
        if (is_shutdown_requested())
            printf("🛑 Shutdown requested during document end\n");
        return result != 0 ? result : -1;
    }

    return 0;
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

    // Set up comprehensive signal handlers FIRST
    setup_signal_handling();

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

    // Check for early shutdown
    if (is_shutdown_requested())
    {
        printf("🛑 Shutdown requested during initialization\n");
        goto cleanup;
    }

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

    // Check for shutdown after config
    if (is_shutdown_requested())
    {
        printf("🛑 Shutdown requested after configuration\n");
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

    // Store context globally for signal handler
    g_context = ctx;

    printf("DEBUG: About to initialize plugins with context\n");
    fflush(stdout);

    // Check shutdown before plugin initialization
    if (is_shutdown_requested())
    {
        printf("🛑 Shutdown requested before plugin initialization\n");
        goto cleanup;
    }

    plugin_manager_initialize_plugins(g_plugin_manager, ctx);
    printf("DEBUG: Plugin initialization completed\n");
    fflush(stdout);

    // Check shutdown after plugin initialization
    if (is_shutdown_requested())
    {
        printf("🛑 Shutdown requested after plugin initialization\n");
        goto cleanup;
    }

    ctx->log(ctx, LOG_DEBUG, "Processing directory: %s\n", config->input_directory);
    ctx->log(ctx, LOG_DEBUG, "Output file: %s\n", config->output_file);
    ctx->log(ctx, LOG_DEBUG, "Format: %s\n", config->output_format);
    ctx->log(ctx, LOG_DEBUG, "Plugins: %d loaded\n", g_plugin_manager->registry.count);

    // Begin processing with shutdown checks
    ctx->log(ctx, LOG_DEBUG, "Beginning processing");

    // NO MORE ALARM CALLS - let it run naturally
    result = safe_process_with_shutdown_check(ctx, config);

    if (result == 0 && !is_shutdown_requested())
    {
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

        // Interactive mode (only if not shutting down)
        if (config->interactive && !is_shutdown_requested())
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
    }
    else if (is_shutdown_requested())
    {
        printf("🛑 Processing interrupted by user\n");
        result = EXIT_FAILURE;
    }

cleanup:
    // Clear global context reference
    g_context = NULL;

    // IMPORTANT: Cleanup plugin manager first with proper context
    if (g_plugin_manager)
    {
        printf("🔌 Cleaning up plugins...\n");
        fflush(stdout);

        plugin_manager_destroy(g_plugin_manager, ctx);
        g_plugin_manager = NULL;
    }

    if (ctx)
    {
        destroy_fconcat_context(ctx);
        ctx = NULL;
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