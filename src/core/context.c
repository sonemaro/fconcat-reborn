#include "context.h"
#include "version.h"
#include "../plugins/plugin.h"
#include "../filter/filter.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

// Maximum depth for symlink cycle detection
#define MAX_VISITED_DIRS 256

// Visited directory tracking for circular symlink detection
typedef struct {
    dev_t dev;
    ino_t ino;
} VisitedInode;

typedef struct {
    VisitedInode inodes[MAX_VISITED_DIRS];
    int count;
} VisitedSet;

// Check if an inode has been visited (returns 1 if already visited)
static int visited_set_contains(VisitedSet *set, dev_t dev, ino_t ino)
{
    if (!set) return 0;
    for (int i = 0; i < set->count; i++) {
        if (set->inodes[i].dev == dev && set->inodes[i].ino == ino) {
            return 1;
        }
    }
    return 0;
}

// Add an inode to the visited set (returns 0 on success, -1 if full)
static int visited_set_add(VisitedSet *set, dev_t dev, ino_t ino)
{
    if (!set || set->count >= MAX_VISITED_DIRS) return -1;
    set->inodes[set->count].dev = dev;
    set->inodes[set->count].ino = ino;
    set->count++;
    return 0;
}

// Remove the last inode from the visited set (for backtracking)
static void visited_set_pop(VisitedSet *set)
{
    if (set && set->count > 0) {
        set->count--;
    }
}

// ============================================================================
// ITERATIVE DIRECTORY STACK - Eliminates recursive stack overflow risk
// ============================================================================

#define DIR_STACK_INITIAL_CAPACITY 256

typedef struct {
    char path[MAX_PATH];           // Full path to directory
    char relative_path[MAX_PATH];  // Relative path from base
    DIR *dir;                      // Open directory handle
    int level;                     // Current depth level
    ino_t inode;                   // For visited set cleanup on pop
    dev_t dev;                     // Device ID for visited set
} DirStackEntry;

typedef struct {
    DirStackEntry *entries;
    int size;
    int capacity;
} DirStack;

static DirStack *dir_stack_create(void)
{
    DirStack *stack = calloc(1, sizeof(DirStack));
    if (!stack) return NULL;
    
    stack->entries = calloc(DIR_STACK_INITIAL_CAPACITY, sizeof(DirStackEntry));
    if (!stack->entries) {
        free(stack);
        return NULL;
    }
    stack->capacity = DIR_STACK_INITIAL_CAPACITY;
    stack->size = 0;
    return stack;
}

static void dir_stack_destroy(DirStack *stack)
{
    if (!stack) return;
    // Close any remaining open directories
    for (int i = 0; i < stack->size; i++) {
        if (stack->entries[i].dir) {
            closedir(stack->entries[i].dir);
        }
    }
    free(stack->entries);
    free(stack);
}

static int dir_stack_push(DirStack *stack, const char *path, const char *rel_path, 
                          DIR *dir, int level, dev_t dev, ino_t inode)
{
    if (!stack || stack->size >= stack->capacity) return -1;
    
    DirStackEntry *entry = &stack->entries[stack->size];
    snprintf(entry->path, MAX_PATH, "%s", path);
    snprintf(entry->relative_path, MAX_PATH, "%s", rel_path);
    entry->dir = dir;
    entry->level = level;
    entry->dev = dev;
    entry->inode = inode;
    stack->size++;
    return 0;
}

static DirStackEntry *dir_stack_peek(DirStack *stack)
{
    if (!stack || stack->size == 0) return NULL;
    return &stack->entries[stack->size - 1];
}

static void dir_stack_pop(DirStack *stack)
{
    if (stack && stack->size > 0) {
        stack->size--;
    }
}

static int dir_stack_is_empty(DirStack *stack)
{
    return (!stack || stack->size == 0);
}

// Helper function to build full path
static int build_full_path(char *full_path, size_t max_len, const char *base_path, const char *relative_path)
{
    if (!full_path || !base_path || !relative_path)
        return -1;

    if (strlen(relative_path) == 0)
    {
        int ret = snprintf(full_path, max_len, "%s", base_path);
        return (ret < 0 || ret >= (int)max_len) ? -1 : 0;
    }
    else
    {
        int ret = snprintf(full_path, max_len, "%s/%s", base_path, relative_path);
        return (ret < 0 || ret >= (int)max_len) ? -1 : 0;
    }
}

// Helper function to build relative path
static int build_relative_path(char *rel_path, size_t max_len, const char *current_rel, const char *name)
{
    if (!rel_path || !current_rel || !name)
        return -1;

    if (strlen(current_rel) == 0)
    {
        int ret = snprintf(rel_path, max_len, "%s", name);
        return (ret < 0 || ret >= (int)max_len) ? -1 : 0;
    }
    else
    {
        int ret = snprintf(rel_path, max_len, "%s/%s", current_rel, name);
        return (ret < 0 || ret >= (int)max_len) ? -1 : 0;
    }
}

// FIXED: Enhanced symlink resolution with proper error handling
static char *resolve_symlink_safely(FconcatContext *ctx, const char *path, SymlinkHandling handling)
{
    if (!path || handling != SYMLINK_FOLLOW)
        return NULL;

    char *resolved = realpath(path, NULL);
    if (!resolved)
    {
        ctx->warning(ctx, "Cannot resolve symlink: %s - %s", path, strerror(errno));
        return NULL;
    }

    return resolved;
}

// Internal traverse function with ITERATIVE stack-based traversal
// This eliminates recursive stack overflow risk (~8KB per frame * 256 depth = 2MB)
static int traverse_directory_internal(FconcatContext *ctx, const char *base_path, const char *relative_path,
                                        int level, DirectoryCallback *callback, VisitedSet *visited)
{
    if (!ctx || !base_path || !relative_path || !callback)
        return -1;

    // Create explicit stack for iterative traversal
    DirStack *stack = dir_stack_create();
    if (!stack) {
        ctx->error(ctx, "Failed to allocate directory stack");
        return -1;
    }

    int result = 0;
    char initial_full_path[MAX_PATH];
    
    if (build_full_path(initial_full_path, sizeof(initial_full_path), base_path, relative_path) != 0) {
        ctx->error(ctx, "Path too long: %s/%s", base_path, relative_path);
        dir_stack_destroy(stack);
        return -1;
    }

    // Get initial directory inode
    struct stat initial_st;
    if (stat(initial_full_path, &initial_st) != 0) {
        ctx->warning(ctx, "Cannot stat directory: %s - %s", initial_full_path, strerror(errno));
        dir_stack_destroy(stack);
        return 0;
    }

    // Open initial directory
    DIR *initial_dir = opendir(initial_full_path);
    if (!initial_dir) {
        if (errno == EACCES) {
            ctx->warning(ctx, "Permission denied accessing directory: %s", initial_full_path);
        } else {
            ctx->warning(ctx, "Cannot open directory: %s - %s", initial_full_path, strerror(errno));
        }
        dir_stack_destroy(stack);
        return 0;
    }

    // Add initial directory to visited set
    visited_set_add(visited, initial_st.st_dev, initial_st.st_ino);

    // Push initial directory onto stack
    if (dir_stack_push(stack, initial_full_path, relative_path, initial_dir, level, 
                       initial_st.st_dev, initial_st.st_ino) != 0) {
        closedir(initial_dir);
        dir_stack_destroy(stack);
        return -1;
    }

    // Iterative traversal loop
    while (!dir_stack_is_empty(stack)) {
        DirStackEntry *current = dir_stack_peek(stack);
        struct dirent *entry = readdir(current->dir);

        if (!entry) {
            // Directory exhausted - pop and continue
            closedir(current->dir);
            current->dir = NULL;
            visited_set_pop(visited);
            dir_stack_pop(stack);
            continue;
        }

        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // SAFETY: Check depth limit
        if (current->level >= MAX_DIRECTORY_DEPTH) {
            ctx->warning(ctx, "Maximum directory depth (%d) exceeded, skipping deeper entries",
                         MAX_DIRECTORY_DEPTH);
            continue;
        }

        char entry_full_path[MAX_PATH];
        char entry_rel_path[MAX_PATH];

        if (build_full_path(entry_full_path, sizeof(entry_full_path), current->path, entry->d_name) != 0) {
            ctx->warning(ctx, "Path too long, skipping: %s", entry->d_name);
            continue;
        }

        if (build_relative_path(entry_rel_path, sizeof(entry_rel_path), current->relative_path, entry->d_name) != 0) {
            ctx->warning(ctx, "Relative path too long, skipping: %s", entry->d_name);
            continue;
        }

        struct stat st;
        if (lstat(entry_full_path, &st) != 0) {
            if (errno == EACCES) {
                ctx->warning(ctx, "Permission denied accessing: %s", entry_full_path);
            } else if (errno == ENOENT) {
                ctx->warning(ctx, "File disappeared during processing: %s", entry_full_path);
            } else {
                ctx->warning(ctx, "Cannot stat: %s - %s", entry_full_path, strerror(errno));
            }
            continue;
        }

        // Create FileInfo structure
        FileInfo file_info = {0};
        file_info.path = entry_rel_path;
        file_info.size = st.st_size;
        file_info.modified_time = st.st_mtime;
        file_info.is_directory = S_ISDIR(st.st_mode);
        file_info.is_symlink = S_ISLNK(st.st_mode);
        file_info.is_binary = false;
        file_info.permissions = st.st_mode;

        // Handle symlinks
        char *resolved_path = NULL;
        if (file_info.is_symlink) {
            const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;
            if (config->symlink_handling == SYMLINK_FOLLOW) {
                resolved_path = resolve_symlink_safely(ctx, entry_full_path, config->symlink_handling);
                if (resolved_path) {
                    struct stat resolved_st;
                    if (stat(resolved_path, &resolved_st) == 0) {
                        file_info.is_directory = S_ISDIR(resolved_st.st_mode);
                        file_info.size = resolved_st.st_size;
                    } else {
                        ctx->warning(ctx, "Cannot stat symlink target: %s", resolved_path);
                        free(resolved_path);
                        resolved_path = NULL;
                    }
                }
            }
        }

        // Check filters
        InternalContextState *internal = (InternalContextState *)ctx->internal_state;
        if (!filter_engine_should_include_path(internal->filter_engine, ctx, entry_rel_path, &file_info)) {
            ctx->log(ctx, LOG_DEBUG, "Excluding path: %s", entry_rel_path);
            if (resolved_path) free(resolved_path);
            continue;
        }

        // Update context
        ctx->current_file_path = entry_rel_path;
        ctx->current_file_info = &file_info;
        ctx->current_directory_level = current->level;

        EntryType entry_type = file_info.is_directory ? ENTRY_TYPE_DIRECTORY : ENTRY_TYPE_FILE;

        // Check binary
        if (entry_type == ENTRY_TYPE_FILE && !file_info.is_symlink) {
            file_info.is_binary = (filter_is_binary_file(entry_full_path) == 1);
        }

        // Callback
        int callback_result = callback->handle_entry(ctx, entry_rel_path, entry_type, &file_info, 
                                                     current->level, callback->user_data);
        ctx->current_file_info = NULL;

        if (callback_result != 0) {
            result = callback_result;
            if (resolved_path) free(resolved_path);
            break;
        }

        // Handle subdirectories - push onto stack instead of recursing
        if (entry_type == ENTRY_TYPE_DIRECTORY) {
            const char *subdir_path = resolved_path ? resolved_path : entry_full_path;
            
            struct stat subdir_st;
            if (stat(subdir_path, &subdir_st) != 0) {
                ctx->warning(ctx, "Cannot stat subdirectory: %s", subdir_path);
                if (resolved_path) free(resolved_path);
                continue;
            }

            // Check for cycles
            if (visited_set_contains(visited, subdir_st.st_dev, subdir_st.st_ino)) {
                ctx->warning(ctx, "Circular symlink detected, skipping: %s", subdir_path);
                if (resolved_path) free(resolved_path);
                continue;
            }

            DIR *subdir = opendir(subdir_path);
            if (!subdir) {
                if (errno == EACCES) {
                    ctx->warning(ctx, "Permission denied accessing directory: %s", subdir_path);
                } else {
                    ctx->warning(ctx, "Cannot open directory: %s - %s", subdir_path, strerror(errno));
                }
                if (resolved_path) free(resolved_path);
                continue;
            }

            visited_set_add(visited, subdir_st.st_dev, subdir_st.st_ino);

            if (dir_stack_push(stack, subdir_path, entry_rel_path, subdir, current->level + 1,
                               subdir_st.st_dev, subdir_st.st_ino) != 0) {
                closedir(subdir);
                visited_set_pop(visited);
                ctx->warning(ctx, "Directory stack full, skipping: %s", subdir_path);
            }
        }

        if (resolved_path) free(resolved_path);
    }

    dir_stack_destroy(stack);
    return result;
}

// Public traverse_directory function - creates visited set and calls internal
int traverse_directory(FconcatContext *ctx, const char *base_path, const char *relative_path,
                       int level, DirectoryCallback *callback)
{
    VisitedSet visited = {0};
    return traverse_directory_internal(ctx, base_path, relative_path, level, callback, &visited);
}

// Structure processing callback
static int structure_callback(FconcatContext *ctx, const char *path, EntryType type,
                              FileInfo *info, int level, void *user_data)
{
    (void)user_data; // Mark as intentionally unused

    InternalContextState *internal = (InternalContextState *)ctx->internal_state;

    if (type == ENTRY_TYPE_DIRECTORY)
    {
        // Write directory entry
        if (internal->format_engine)
        {
            return format_engine_write_directory(internal->format_engine, ctx, path, level);
        }
    }
    else
    {
        // Write file entry
        if (internal->format_engine)
        {
            return format_engine_write_file_entry(internal->format_engine, ctx, path, info);
        }
    }

    return 0;
}

// Content processing callback - FIXED: Removed unused parameters
static int content_callback(FconcatContext *ctx, const char *path, EntryType type,
                            FileInfo *info, int level, void *user_data)
{
    (void)level;     // Mark as intentionally unused
    (void)user_data; // Mark as intentionally unused

    if (type == ENTRY_TYPE_DIRECTORY)
    {
        return 0; // Skip directories in content processing
    }

    // Get internal state to access engines
    InternalContextState *internal = (InternalContextState *)ctx->internal_state;

    // Process file content
    ctx->log(ctx, LOG_DEBUG, "Processing file: %s", path);

    // Update file count in stats
    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    if (stats)
    {
        stats->processed_files++;
        stats->total_files++;
    }

    // Write file header
    if (internal->format_engine)
    {
        int result = format_engine_write_file_header(internal->format_engine, ctx, path);
        if (result != 0)
            return result;
    }

    // Build full path for file access
    char full_path[MAX_PATH];
    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;
    if (build_full_path(full_path, sizeof(full_path), config->input_directory, path) != 0)
    {
        ctx->error(ctx, "Path too long: %s", path);
        return -1;
    }

    // SAFETY: Check file size limit to prevent resource exhaustion
    if (info->size > MAX_FILE_SIZE)
    {
        ctx->warning(ctx, "File too large, skipping (limit %lluMB): %s (%zu bytes)",
                     (unsigned long long)(MAX_FILE_SIZE / (1024 * 1024)), path, info->size);
        if (stats)
        {
            stats->skipped_files++;
        }
        return 0; // Continue with other files
    }

    // FIXED: Graceful file opening with permission handling
    FILE *file = fopen(full_path, "rb");
    if (!file)
    {
        if (errno == EACCES)
        {
            ctx->warning(ctx, "Permission denied opening file: %s", full_path);
        }
        else if (errno == ENOENT)
        {
            ctx->warning(ctx, "File disappeared during processing: %s", full_path);
        }
        else
        {
            ctx->warning(ctx, "Cannot open file: %s - %s", full_path, strerror(errno));
        }
        return 0; // Continue processing other files
    }

    // Determine optimal buffer size based on file size
    size_t buffer_size;
    if (info->size > 0 && info->size < 4096)
    {
        // Small file - use file size as buffer size
        buffer_size = info->size;
    }
    else if (info->size < 16384)
    {
        // Medium file - use 4KB buffer
        buffer_size = 4096;
    }
    else
    {
        // Large file - use 16KB buffer
        buffer_size = 16384;
    }

    // Get buffer from pool
    char *buffer = memory_get_buffer(internal->memory_manager, buffer_size);
    if (!buffer)
    {
        ctx->error(ctx, "Failed to allocate buffer for file: %s", full_path);
        fclose(file);
        return -1;
    }

    // Read file content in chunks
    size_t bytes_read;
    bool content_excluded = false;

    while ((bytes_read = fread(buffer, 1, buffer_size, file)) > 0)
    {
        // Check if content should be included
        if (!filter_engine_should_include_content(internal->filter_engine, ctx, path, buffer, bytes_read))
        {
            ctx->log(ctx, LOG_DEBUG, "Excluding content for: %s", path);
            // Still count as processed but mark as skipped
            if (stats)
            {
                stats->skipped_files++;
                stats->processed_files--; // Subtract from processed count
            }
            content_excluded = true;
            break;
        }

        // Transform content through filter engine
        char *transformed_data = NULL;
        size_t transformed_size = 0;

        if (filter_engine_transform_content(internal->filter_engine, ctx, path,
                                            buffer, bytes_read, &transformed_data, &transformed_size) == 0)
        {
            // Use transformed data
            if (internal->format_engine)
            {
                format_engine_write_file_chunk(internal->format_engine, ctx, transformed_data, transformed_size);
            }
            if (stats)
            {
                stats->filtered_bytes += transformed_size;
            }

            // Release transformed data buffer back to pool
            memory_release_buffer(internal->memory_manager, transformed_data);
        }
        else
        {
            // Use original data
            if (internal->format_engine)
            {
                format_engine_write_file_chunk(internal->format_engine, ctx, buffer, bytes_read);
            }
            if (stats)
            {
                stats->processed_bytes += bytes_read;
            }
        }

        // Update progress
        update_context_progress(ctx, bytes_read);
    }

    // Release buffer back to pool
    memory_release_buffer(internal->memory_manager, buffer);
    fclose(file);

    // Write file footer (only if content wasn't excluded)
    if (!content_excluded && internal->format_engine)
    {
        format_engine_write_file_footer(internal->format_engine, ctx);
    }

    return 0;
}

int process_directory_structure(FconcatContext *ctx, const char *base_path, const char *relative_path, int level)
{
    DirectoryCallback callback = {
        .handle_entry = structure_callback,
        .user_data = NULL};

    return traverse_directory(ctx, base_path, relative_path, level, &callback);
}

int process_directory_content(FconcatContext *ctx, const char *base_path, const char *relative_path, int level)
{
    DirectoryCallback callback = {
        .handle_entry = content_callback,
        .user_data = NULL};

    return traverse_directory(ctx, base_path, relative_path, level, &callback);
}

FconcatContext *create_fconcat_context(const ResolvedConfig *config,
                                       FILE *output_file,
                                       ProcessingStats *stats,
                                       ErrorManager *error_manager,
                                       MemoryManager *memory_manager,
                                       struct PluginManager *plugin_manager,
                                       struct FormatEngine *format_engine,
                                       struct FilterEngine *filter_engine)
{
    // Use heap allocation for context to ensure it's properly isolated
    FconcatContext *ctx = calloc(1, sizeof(FconcatContext));
    if (!ctx)
        return NULL;

    InternalContextState *internal_state = calloc(1, sizeof(InternalContextState));
    if (!internal_state)
    {
        free(ctx);
        return NULL;
    }

    // Initialize internal state
    internal_state->output_file = output_file;
    internal_state->config = config;
    internal_state->stats = stats;
    internal_state->error_manager = error_manager;
    internal_state->memory_manager = memory_manager;
    internal_state->plugin_manager = plugin_manager;
    internal_state->format_engine = format_engine;
    internal_state->filter_engine = filter_engine;
    internal_state->progress_callback = NULL;
    internal_state->progress_user_data = NULL;

    // Initialize context with function pointers
    ctx->config = (const void *)config;
    ctx->get_config_string = context_get_config_string;
    ctx->get_config_int = context_get_config_int;
    ctx->get_config_bool = context_get_config_bool;

    ctx->get_plugin_parameter = context_get_plugin_parameter;
    ctx->get_plugin_parameter_count = context_get_plugin_parameter_count;
    ctx->get_plugin_parameter_by_index = context_get_plugin_parameter_by_index;

    ctx->log = context_log;
    ctx->vlog = context_vlog;
    ctx->is_log_enabled = context_is_log_enabled;

    ctx->alloc = context_alloc;
    ctx->realloc = context_realloc;
    ctx->free = context_free;

    ctx->write_output = context_write_output;
    ctx->write_output_fmt = context_write_output_fmt;

    ctx->error = context_error;
    ctx->warning = context_warning;
    ctx->get_error_count = context_get_error_count;

    ctx->current_file_path = NULL;
    ctx->current_file_info = NULL;
    ctx->current_file_processed_bytes = 0;
    ctx->current_directory_level = 0;

    ctx->stats = (void *)stats;

    ctx->progress = context_progress;
    ctx->set_progress_callback = context_set_progress_callback;

    ctx->get_plugin_data = context_get_plugin_data;
    ctx->set_plugin_data = context_set_plugin_data;
    ctx->call_plugin_method = context_call_plugin_method;

    ctx->create_stream_buffer = context_create_stream_buffer;
    ctx->stream_write = context_stream_write;
    ctx->stream_flush = context_stream_flush;
    ctx->stream_destroy = context_stream_destroy;

    ctx->file_exists = context_file_exists;
    ctx->get_file_info = context_get_file_info;
    ctx->resolve_path = context_resolve_path;

    ctx->fconcat_version = FCONCAT_VERSION;
    ctx->build_info = "Debug build";
    ctx->processing_start_time = time(NULL);
    ctx->output_file_path = config->output_file;

    ctx->internal_state = internal_state;

    return ctx;
}

void destroy_fconcat_context(FconcatContext *ctx)
{
    if (!ctx)
        return;

    free(ctx->internal_state);
    free(ctx);
}

void update_context_for_file(FconcatContext *ctx, const char *filepath, const FileInfo *info)
{
    if (!ctx)
        return;

    ctx->current_file_path = filepath;
    ctx->current_file_info = (const void *)info;
    ctx->current_file_processed_bytes = 0;

    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    if (stats)
    {
        stats->processed_files++;
    }
}

void update_context_progress(FconcatContext *ctx, size_t bytes_processed)
{
    if (!ctx)
        return;

    ctx->current_file_processed_bytes += bytes_processed;

    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    if (stats)
    {
        stats->processed_bytes += bytes_processed;
        stats->current_time = time(NULL);
    }
}

const char *context_get_config_string(FconcatContext *ctx, const char *key)
{
    if (!ctx || !ctx->config || !key)
        return NULL;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;

    if (strcmp(key, "output_format") == 0)
    {
        return config->output_format;
    }
    else if (strcmp(key, "input_directory") == 0)
    {
        return config->input_directory;
    }
    else if (strcmp(key, "output_file") == 0)
    {
        return config->output_file;
    }

    return NULL;
}

int context_get_config_int(FconcatContext *ctx, const char *key)
{
    if (!ctx || !ctx->config || !key)
        return 0;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;

    if (strcmp(key, "binary_handling") == 0)
    {
        return config->binary_handling;
    }
    else if (strcmp(key, "symlink_handling") == 0)
    {
        return config->symlink_handling;
    }
    else if (strcmp(key, "log_level") == 0)
    {
        return config->log_level;
    }

    return 0;
}

bool context_get_config_bool(FconcatContext *ctx, const char *key)
{
    if (!ctx || !ctx->config || !key)
        return false;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;

    if (strcmp(key, "show_size") == 0)
    {
        return config->show_size;
    }
    else if (strcmp(key, "verbose") == 0)
    {
        return config->verbose;
    }
    else if (strcmp(key, "interactive") == 0)
    {
        return config->interactive;
    }

    return false;
}

const char *context_get_plugin_parameter(FconcatContext *ctx, const char *plugin_name, const char *param_name)
{
    if (!ctx || !plugin_name || !param_name)
        return NULL;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->plugin_manager)
    {
        return plugin_manager_get_parameter(state->plugin_manager, plugin_name, param_name);
    }

    return NULL;
}

int context_get_plugin_parameter_count(FconcatContext *ctx, const char *plugin_name)
{
    if (!ctx || !plugin_name)
        return 0;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->plugin_manager)
    {
        return plugin_manager_get_parameter_count(state->plugin_manager, plugin_name);
    }

    return 0;
}

const char *context_get_plugin_parameter_by_index(FconcatContext *ctx, const char *plugin_name, int index)
{
    if (!ctx || !plugin_name || index < 0)
        return NULL;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->plugin_manager)
    {
        return plugin_manager_get_parameter_by_index(state->plugin_manager, plugin_name, index);
    }

    return NULL;
}

void context_log(FconcatContext *ctx, LogLevel level, const char *format, ...)
{
    if (!ctx || !format)
        return;

    va_list args;
    va_start(args, format);
    context_vlog(ctx, level, format, args);
    va_end(args);
}

void context_vlog(FconcatContext *ctx, LogLevel level, const char *format, va_list args)
{
    if (!ctx || !format)
        return;

    // FIXED: Respect log level configuration
    if (!context_is_log_enabled(ctx, level))
        return;

    const char *level_str = "UNKNOWN";
    switch (level)
    {
    case LOG_ERROR:
        level_str = "ERROR";
        break;
    case LOG_WARNING:
        level_str = "WARNING";
        break;
    case LOG_INFO:
        level_str = "INFO";
        break;
    case LOG_DEBUG:
        level_str = "DEBUG";
        break;
    case LOG_TRACE:
        level_str = "TRACE";
        break;
    }

    fprintf(stderr, "[%s] ", level_str);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
}

bool context_is_log_enabled(FconcatContext *ctx, LogLevel level)
{
    if (!ctx)
        return false;

    const ResolvedConfig *config = (const ResolvedConfig *)ctx->config;
    if (config)
    {
        return (int)level <= config->log_level;
    }

    return (int)level <= (int)LOG_INFO;
}

void *context_alloc(FconcatContext *ctx, size_t size)
{
    if (!ctx)
        return malloc(size);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        return memory_alloc(state->memory_manager, size);
    }

    return malloc(size);
}

void *context_realloc(FconcatContext *ctx, void *ptr, size_t size)
{
    if (!ctx)
        return realloc(ptr, size);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        return memory_realloc(state->memory_manager, ptr, size);
    }

    return realloc(ptr, size);
}

void context_free(FconcatContext *ctx, void *ptr)
{
    if (!ctx)
    {
        free(ptr);
        return;
    }

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        memory_free(state->memory_manager, ptr);
    }
    else
    {
        free(ptr);
    }
}

int context_write_output(FconcatContext *ctx, const char *data, size_t size)
{
    if (!ctx || !data)
        return -1;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->output_file)
    {
        if (size == 0)
            size = strlen(data);
        return fwrite(data, 1, size, state->output_file) == size ? 0 : -1;
    }

    return -1;
}

int context_write_output_fmt(FconcatContext *ctx, const char *format, ...)
{
    if (!ctx || !format)
        return -1;

    va_list args;
    va_start(args, format);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->output_file)
    {
        int result = vfprintf(state->output_file, format, args);
        va_end(args);
        return result;
    }

    va_end(args);
    return -1;
}

void context_error(FconcatContext *ctx, const char *format, ...)
{
    if (!ctx || !format)
        return;

    va_list args;
    va_start(args, format);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->error_manager)
    {
        context_vlog(ctx, LOG_ERROR, format, args);
    }

    va_end(args);
}

void context_warning(FconcatContext *ctx, const char *format, ...)
{
    if (!ctx || !format)
        return;

    va_list args;
    va_start(args, format);

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->error_manager)
    {
        context_vlog(ctx, LOG_WARNING, format, args);
    }

    va_end(args);
}

int context_get_error_count(FconcatContext *ctx)
{
    if (!ctx)
        return 0;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->error_manager)
    {
        return error_get_count(state->error_manager);
    }

    return 0;
}

void context_progress(FconcatContext *ctx, const char *operation, size_t current, size_t total)
{
    if (!ctx)
        return;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->progress_callback)
    {
        state->progress_callback(operation, current, total, state->progress_user_data);
    }
}

void context_set_progress_callback(FconcatContext *ctx, ProgressCallback callback, void *user_data)
{
    if (!ctx)
        return;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state)
    {
        state->progress_callback = callback;
        state->progress_user_data = user_data;
    }
}

void *context_get_plugin_data(FconcatContext *ctx, const char *plugin_name)
{
    if (!ctx || !plugin_name)
        return NULL;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->plugin_manager)
    {
        return plugin_manager_get_plugin_data(state->plugin_manager, plugin_name);
    }

    return NULL;
}

int context_set_plugin_data(FconcatContext *ctx, const char *plugin_name, void *data, size_t size)
{
    if (!ctx || !plugin_name || !data)
        return -1;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->plugin_manager)
    {
        return plugin_manager_set_plugin_data(state->plugin_manager, plugin_name, data, size);
    }

    return -1;
}

int context_call_plugin_method(FconcatContext *ctx, const char *plugin_name, const char *method, void *args)
{
    if (!ctx || !plugin_name || !method)
        return -1;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->plugin_manager)
    {
        return plugin_manager_call_plugin_method(state->plugin_manager, plugin_name, method, args);
    }

    return -1;
}

void *context_create_stream_buffer(FconcatContext *ctx, size_t initial_size)
{
    if (!ctx)
        return NULL;

    InternalContextState *state = (InternalContextState *)ctx->internal_state;
    if (state && state->memory_manager)
    {
        return (void *)stream_buffer_create(state->memory_manager, initial_size);
    }

    return NULL;
}

int context_stream_write(FconcatContext *ctx, void *buffer, const char *data, size_t size)
{
    (void)ctx;
    return stream_buffer_write((StreamBuffer *)buffer, data, size);
}

int context_stream_flush(FconcatContext *ctx, void *buffer)
{
    (void)ctx;
    return stream_buffer_flush((StreamBuffer *)buffer);
}

void context_stream_destroy(FconcatContext *ctx, void *buffer)
{
    (void)ctx;
    stream_buffer_destroy((StreamBuffer *)buffer);
}

bool context_file_exists(FconcatContext *ctx, const char *path)
{
    (void)ctx;
    if (!path)
        return false;

    struct stat st;
    return lstat(path, &st) == 0;  // lstat to not follow symlinks
}

/**
 * @brief Get file information for a path
 * 
 * @param ctx The context (unused but required for API consistency)
 * @param path The file path to query
 * @param info Pointer to FileInfo structure to populate
 * @return 0 on success, -1 on failure
 * 
 * @note IMPORTANT: Caller takes ownership of file_info->path and must free() it.
 *       The path field is allocated with strdup() and the caller is responsible
 *       for freeing it when done with the FileInfo structure.
 */
int context_get_file_info(FconcatContext *ctx, const char *path, void *info)
{
    (void)ctx;
    if (!path || !info)
        return -1;

    FileInfo *file_info = (FileInfo *)info;
    struct stat st;
    // Use lstat to detect symlinks - stat() follows them and can't detect them
    if (lstat(path, &st) != 0)
    {
        return -1;
    }

    file_info->path = strdup(path);
    if (!file_info->path)
        return -1;

    file_info->size = st.st_size;
    file_info->modified_time = st.st_mtime;
    file_info->is_directory = S_ISDIR(st.st_mode);
    file_info->is_symlink = S_ISLNK(st.st_mode);
    file_info->is_binary = false; // Would need binary detection
    file_info->permissions = st.st_mode;

    return 0;
}

char *context_resolve_path(FconcatContext *ctx, const char *relative_path)
{
    (void)ctx;
    if (!relative_path)
        return NULL;

    return strdup(relative_path);
}