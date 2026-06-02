#include "fconcat.h"
#include "core/context.h"
#include "filter/filter.h"
#include "output/output.h"
#include "server/server.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int signum)
{
    (void)signum;
    g_shutdown_requested++;
    if (g_shutdown_requested >= 2)
        _exit(130);
}

static int shutdown_requested(void *user_data)
{
    (void)user_data;
    return g_shutdown_requested > 0;
}

static void setup_signal_handling(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void print_header(void)
{
    printf("fconcat %s - safe streaming file concatenator\n", FCONCAT_VERSION);
    printf("%s\n", FCONCAT_COPYRIGHT);
}

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s <input_directory> <output_file> [options]\n"
            "  %s --serve --listen <host:port> --allow-root <path> [server options]\n"
            "\n"
            "Batch options:\n"
            "  --include <patterns>        Include only files matching patterns.\n"
            "  --exclude <patterns>        Exclude files matching patterns.\n"
            "  --show-size, -s             Show file sizes in the tree.\n"
            "  --binary-skip               Skip binary files entirely (default).\n"
            "  --binary-include            Include binary file bytes.\n"
            "  --binary-placeholder        Emit a placeholder for binary file content.\n"
            "  --symlinks <mode>           skip, follow, include, placeholder.\n"
            "  --verbose, -v               Enable debug logging.\n"
            "  --log-level <level>         error, warning, info, debug, trace.\n"
            "\n"
            "Server options:\n"
            "  --listen <host:port>        Address to bind, for example 127.0.0.1:8080.\n"
            "  --allow-root <path>         Allowed root; repeat for multiple roots.\n"
            "  --workers <n>               Worker threads (default %d).\n"
            "  --queue <n>                 Pending connection queue (default %d).\n"
            "  --auth-token <token>        Optional bearer token for all requests.\n"
            "\n"
            "Server API:\n"
            "  GET /healthz\n"
            "  GET /concat?root=<path>&include=<glob>&exclude=<glob>&show_size=1\n"
            "      &binary=skip|include|placeholder&symlinks=skip|follow|include|placeholder\n",
            program_name, program_name, DEFAULT_SERVER_WORKERS, DEFAULT_SERVER_QUEUE);
}

static int run_batch(const ResolvedConfig *config)
{
    struct stat st;
    if (!config || !config->input_directory || !config->output_file)
        return -1;
    if (stat(config->input_directory, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "Invalid input directory: %s\n", config->input_directory);
        return -1;
    }

    int output_fd = open(config->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output_fd < 0)
    {
        fprintf(stderr, "Cannot open output file %s: %s\n", config->output_file, strerror(errno));
        return -1;
    }

    FdOutputSink fd_sink;
    fd_output_sink_init(&fd_sink, output_fd, 1);
    BufferedOutputSink buffered_sink;
    OutputSink *output_sink = &fd_sink.sink;
    int using_buffered_sink = buffered_output_sink_init(&buffered_sink, &fd_sink.sink, 0, 1) == 0;
    if (using_buffered_sink)
        output_sink = &buffered_sink.sink;

    ErrorManager *errors = error_manager_create();
    MemoryManager *memory = memory_manager_create();
    FilterEngine *filters = filter_engine_create();
    int result = -1;

    if (!errors || !memory || !filters)
        goto cleanup;

    if (filter_engine_configure(filters, config) != 0)
        goto cleanup;

    ProcessingStats stats = {0};
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    stats.start_time = (double)start_time.tv_sec + start_time.tv_nsec / 1000000000.0;

    FconcatContext *ctx = create_fconcat_context(config, output_sink, &stats, errors, memory, filters);
    if (!ctx)
        goto cleanup;

    result = process_fconcat_document(ctx, config, shutdown_requested, NULL);
    destroy_fconcat_context(ctx);

    if (output_sink_close(output_sink) != 0)
        result = -1;
    if (using_buffered_sink)
        buffered_output_sink_destroy(&buffered_sink);
    using_buffered_sink = 0;

    if (result == 0 && !shutdown_requested(NULL))
    {
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                         (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;
        MemoryStats memory_stats = memory_get_stats(memory);
        printf("Processing completed successfully.\n");
        printf("Processing time: %.3f seconds\n", elapsed);
        printf("Files processed: %zu\n", stats.processed_files);
        printf("Bytes processed: %zu\n", stats.processed_bytes);
        printf("Tracked peak memory: %zu bytes\n", memory_stats.peak_usage);
    }

cleanup:
    if (filters)
        filter_engine_destroy(filters);
    if (memory)
        memory_manager_destroy(memory);
    if (errors)
        error_manager_destroy(errors);
    if (using_buffered_sink)
    {
        output_sink_close(&buffered_sink.sink);
        buffered_output_sink_destroy(&buffered_sink);
    }
    else if (fd_sink.fd >= 0)
    {
        output_sink_close(&fd_sink.sink);
    }
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        print_header();
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0))
    {
        printf("fconcat %s\n", FCONCAT_VERSION);
        return EXIT_SUCCESS;
    }

    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    setup_signal_handling();

    ConfigManager *config_manager = config_manager_create();
    if (!config_manager)
    {
        fprintf(stderr, "Failed to create configuration manager\n");
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_FAILURE;
    if (config_load_defaults(config_manager) != 0 ||
        config_load_cli(config_manager, argc, argv) != 0)
    {
        fprintf(stderr, "Invalid arguments.\n\n");
        print_usage(argv[0]);
        goto cleanup;
    }

    ResolvedConfig *config = config_resolve(config_manager);
    if (!config)
    {
        fprintf(stderr, "Invalid configuration.\n\n");
        print_usage(argv[0]);
        goto cleanup;
    }

    if (config->mode == FCONCAT_MODE_SERVER)
        exit_code = server_run(config, shutdown_requested, NULL) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    else
        exit_code = run_batch(config);

cleanup:
    config_manager_destroy(config_manager);
    return exit_code;
}
