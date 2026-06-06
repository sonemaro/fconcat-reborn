#include "file_index.h"
#include "context.h"
#include "../filter/filter.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARENA_CHUNK_SIZE (64U * 1024U)
#define FILE_INDEX_INITIAL_CAPACITY 1024U
#define DIR_STACK_INITIAL_CAPACITY 256U
#define MAX_VISITED_DIRS 256
#define FILE_INDEX_SMALL_CACHE_LIMIT (8ULL * 1024ULL)

typedef struct ArenaChunk
{
    struct ArenaChunk *next;
    size_t capacity;
    size_t used;
    unsigned char data[];
} ArenaChunk;

typedef struct
{
    ArenaChunk *head;
} Arena;

struct FileIndex
{
    FileIndexEntry *entries;
    size_t count;
    size_t capacity;
    Arena arena;
    size_t prefix_budget;
    size_t prefix_used;
    size_t prefix_file_limit;
    int prefix_cache_disabled;
};

static int file_index_entries_are_valid(const FileIndex *index)
{
    return index &&
           index->count <= index->capacity &&
           index->capacity <= SIZE_MAX / sizeof(FileIndexEntry) &&
           (index->capacity == 0 || index->entries);
}

typedef struct
{
    dev_t dev;
    ino_t ino;
} VisitedInode;

typedef struct
{
    VisitedInode inodes[MAX_VISITED_DIRS];
    int count;
} VisitedSet;

typedef struct
{
    char path[MAX_PATH];
    char relative_path[MAX_PATH];
    struct dirent **entries;
    int entry_count;
    int next_entry;
    int level;
    int visited_tracked;
} DirStackEntry;

typedef struct
{
    char *data;
    size_t size;
    int complete;
} PrefixProbe;

typedef struct
{
    DirStackEntry *entries;
    int size;
    int capacity;
} DirStack;

static void arena_destroy(Arena *arena)
{
    if (!arena)
        return;

    ArenaChunk *chunk = arena->head;
    while (chunk)
    {
        ArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    arena->head = NULL;
}

static void *arena_alloc(Arena *arena, size_t size, size_t alignment)
{
    if (!arena)
        return NULL;
    if (size == 0)
        size = 1;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);

    ArenaChunk *chunk = arena->head;
    if (chunk)
    {
        size_t padding = (alignment - (chunk->used % alignment)) % alignment;
        if (chunk->used <= chunk->capacity &&
            padding <= chunk->capacity - chunk->used &&
            size <= chunk->capacity - chunk->used - padding)
        {
            void *ptr = chunk->data + chunk->used + padding;
            chunk->used += padding + size;
            return ptr;
        }
    }

    size_t capacity = ARENA_CHUNK_SIZE;
    if (size > capacity)
    {
        if (size > SIZE_MAX - alignment)
            return NULL;
        capacity = size + alignment;
    }

    if (capacity > SIZE_MAX - sizeof(ArenaChunk))
        return NULL;

    ArenaChunk *new_chunk = malloc(sizeof(ArenaChunk) + capacity);
    if (!new_chunk)
        return NULL;

    new_chunk->next = arena->head;
    new_chunk->capacity = capacity;
    new_chunk->used = 0;
    arena->head = new_chunk;

    size_t padding = (alignment - (new_chunk->used % alignment)) % alignment;
    void *ptr = new_chunk->data + new_chunk->used + padding;
    new_chunk->used += padding + size;
    return ptr;
}

static char *arena_strdup(Arena *arena, const char *value)
{
    if (!arena || !value)
        return NULL;

    size_t len = strlen(value);
    char *copy = arena_alloc(arena, len + 1, 1);
    if (!copy)
        return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

static void dir_entries_free(struct dirent **entries, int count)
{
    if (!entries)
        return;

    for (int i = 0; i < count; i++)
        free(entries[i]);
    free(entries);
}

static int skip_dot_entry(const struct dirent *entry)
{
    return entry &&
           strcmp(entry->d_name, ".") != 0 &&
           strcmp(entry->d_name, "..") != 0;
}

static int compare_dir_entries_by_name(const void *left, const void *right)
{
    const struct dirent *a = *(const struct dirent *const *)left;
    const struct dirent *b = *(const struct dirent *const *)right;
    return strcmp(a->d_name, b->d_name);
}

static struct dirent *dir_entry_dup(const struct dirent *entry)
{
    if (!entry)
        return NULL;

    struct dirent *copy = malloc(sizeof(*copy));
    if (!copy)
        return NULL;
    memcpy(copy, entry, sizeof(*copy));
    return copy;
}

static int next_dir_entry_capacity(int capacity, int *next_capacity)
{
    if (!next_capacity || capacity < 0 || capacity == INT_MAX)
        return -1;

    int candidate;
    if (capacity == 0)
    {
        candidate = 64;
    }
    else if (capacity > INT_MAX / 2)
    {
        candidate = INT_MAX;
    }
    else
    {
        candidate = capacity * 2;
    }

    if (candidate <= capacity ||
        (size_t)candidate > SIZE_MAX / sizeof(struct dirent *))
        return -1;

    *next_capacity = candidate;
    return 0;
}

static int read_sorted_directory(const char *path, struct dirent ***entries, int *entry_count)
{
    if (!path || !entries || !entry_count)
        return -1;

    *entries = NULL;
    *entry_count = 0;

    DIR *dir = opendir(path);
    if (!dir)
        return -1;

    struct dirent **list = NULL;
    int count = 0;
    int capacity = 0;
    int saved_errno = 0;

    for (;;)
    {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry)
        {
            saved_errno = errno;
            break;
        }

        if (!skip_dot_entry(entry))
            continue;

        if (count == capacity)
        {
            int new_capacity = 0;
            if (next_dir_entry_capacity(capacity, &new_capacity) != 0)
            {
                saved_errno = ENOMEM;
                break;
            }

            struct dirent **new_list = realloc(list, (size_t)new_capacity * sizeof(*new_list));
            if (!new_list)
            {
                saved_errno = ENOMEM;
                break;
            }

            list = new_list;
            capacity = new_capacity;
        }

        list[count] = dir_entry_dup(entry);
        if (!list[count])
        {
            saved_errno = ENOMEM;
            break;
        }
        count++;
    }

    if (closedir(dir) != 0 && saved_errno == 0)
        saved_errno = errno;

    if (saved_errno != 0)
    {
        dir_entries_free(list, count);
        errno = saved_errno;
        return -1;
    }

    if (count > 1)
        qsort(list, (size_t)count, sizeof(*list), compare_dir_entries_by_name);

    *entries = list;
    *entry_count = count;
    return 0;
}

static DirStack *dir_stack_create(void)
{
    DirStack *stack = calloc(1, sizeof(*stack));
    if (!stack)
        return NULL;

    stack->entries = calloc(DIR_STACK_INITIAL_CAPACITY, sizeof(*stack->entries));
    if (!stack->entries)
    {
        free(stack);
        return NULL;
    }

    stack->capacity = DIR_STACK_INITIAL_CAPACITY;
    return stack;
}

static void dir_stack_destroy(DirStack *stack)
{
    if (!stack)
        return;

    for (int i = 0; i < stack->size; i++)
        dir_entries_free(stack->entries[i].entries, stack->entries[i].entry_count);
    free(stack->entries);
    free(stack);
}

static int dir_stack_push(DirStack *stack, const char *path, const char *relative_path,
                          struct dirent **entries, int entry_count,
                          int level, int visited_tracked)
{
    if (!stack || !path || !relative_path || entry_count < 0 ||
        (!entries && entry_count > 0) || stack->size >= stack->capacity)
        return -1;

    DirStackEntry *entry = &stack->entries[stack->size];
    int n = snprintf(entry->path, sizeof(entry->path), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(entry->path))
        return -1;

    n = snprintf(entry->relative_path, sizeof(entry->relative_path), "%s", relative_path);
    if (n < 0 || (size_t)n >= sizeof(entry->relative_path))
        return -1;

    entry->entries = entries;
    entry->entry_count = entry_count;
    entry->next_entry = 0;
    entry->level = level;
    entry->visited_tracked = visited_tracked;
    stack->size++;
    return 0;
}

static DirStackEntry *dir_stack_peek(DirStack *stack)
{
    if (!stack || stack->size == 0)
        return NULL;
    return &stack->entries[stack->size - 1];
}

static void dir_stack_pop(DirStack *stack)
{
    if (stack && stack->size > 0)
    {
        DirStackEntry *entry = &stack->entries[stack->size - 1];
        dir_entries_free(entry->entries, entry->entry_count);
        memset(entry, 0, sizeof(*entry));
        stack->size--;
    }
}

static int visited_set_contains(const VisitedSet *set, dev_t dev, ino_t ino)
{
    if (!set)
        return 0;

    for (int i = 0; i < set->count; i++)
    {
        if (set->inodes[i].dev == dev && set->inodes[i].ino == ino)
            return 1;
    }
    return 0;
}

static int visited_set_add(VisitedSet *set, dev_t dev, ino_t ino)
{
    if (!set || set->count >= MAX_VISITED_DIRS)
        return -1;

    set->inodes[set->count].dev = dev;
    set->inodes[set->count].ino = ino;
    set->count++;
    return 0;
}

static void visited_set_pop(VisitedSet *set)
{
    if (set && set->count > 0)
        set->count--;
}

static int build_full_path(char *full_path, size_t max_len, const char *base_path, const char *relative_path)
{
    if (!full_path || !base_path || !relative_path)
        return -1;

    if (relative_path[0] == '\0')
    {
        int n = snprintf(full_path, max_len, "%s", base_path);
        return (n < 0 || (size_t)n >= max_len) ? -1 : 0;
    }

    int n = snprintf(full_path, max_len, "%s/%s", base_path, relative_path);
    return (n < 0 || (size_t)n >= max_len) ? -1 : 0;
}

static int build_relative_path(char *rel_path, size_t max_len, const char *current_rel, const char *name)
{
    if (!rel_path || !current_rel || !name)
        return -1;

    if (current_rel[0] == '\0')
    {
        int n = snprintf(rel_path, max_len, "%s", name);
        return (n < 0 || (size_t)n >= max_len) ? -1 : 0;
    }

    int n = snprintf(rel_path, max_len, "%s/%s", current_rel, name);
    return (n < 0 || (size_t)n >= max_len) ? -1 : 0;
}

static size_t stat_size_to_size_t(off_t size)
{
    if (size <= 0)
        return 0;
    if ((uintmax_t)size > (uintmax_t)SIZE_MAX)
        return SIZE_MAX;
    return (size_t)size;
}

static FileIndexEntry *file_index_append(FileIndex *index,
                                         const char *full_path,
                                         const char *relative_path,
                                         int level,
                                         const FileInfo *info)
{
    if (!file_index_entries_are_valid(index) || !full_path || !relative_path || !info)
        return NULL;

    if (index->count >= index->capacity)
    {
        size_t new_capacity = FILE_INDEX_INITIAL_CAPACITY;
        if (index->capacity > 0)
        {
            if (index->capacity > SIZE_MAX / 2)
                return NULL;
            new_capacity = index->capacity * 2;
        }

        if (new_capacity <= index->capacity || new_capacity > SIZE_MAX / sizeof(FileIndexEntry))
            return NULL;

        FileIndexEntry *entries = realloc(index->entries, new_capacity * sizeof(*entries));
        if (!entries)
            return NULL;
        memset(entries + index->capacity, 0, (new_capacity - index->capacity) * sizeof(*entries));
        index->entries = entries;
        index->capacity = new_capacity;
    }

    char *owned_full_path = arena_strdup(&index->arena, full_path);
    char *owned_relative_path = arena_strdup(&index->arena, relative_path);
    if (!owned_full_path || !owned_relative_path)
        return NULL;

    FileIndexEntry *entry = &index->entries[index->count];
    memset(entry, 0, sizeof(*entry));
    entry->full_path = owned_full_path;
    entry->relative_path = owned_relative_path;
    entry->level = level;
    entry->info = *info;
    entry->info.path = owned_relative_path;
    index->count++;
    return entry;
}

static void account_null_output_file(FconcatContext *ctx, const FileInfo *info)
{
    if (!ctx || !info)
        return;

    ProcessingStats *stats = (ProcessingStats *)ctx->stats;
    if (!stats)
        return;

    fconcat_size_increment_saturated(&stats->processed_files);
    fconcat_size_increment_saturated(&stats->total_files);
    fconcat_size_add_saturated(&stats->total_bytes, info->size);
    if (info->size > MAX_FILE_SIZE)
        fconcat_size_increment_saturated(&stats->skipped_files);
    else
        fconcat_size_add_saturated(&stats->processed_bytes, info->size);
}

static int buffer_has_nul(const char *buffer, size_t size)
{
    if (!buffer)
        return 0;

    for (size_t i = 0; i < size; i++)
    {
        if (buffer[i] == '\0')
            return 1;
    }
    return 0;
}

static void file_index_probe_file(FileIndex *index, const ResolvedConfig *config,
                                  const char *full_path, FileInfo *info,
                                  char *scratch, size_t scratch_size,
                                  PrefixProbe *probe)
{
    if (!index || !config || !full_path || !info || !scratch || scratch_size == 0 || !probe)
        return;
    if (info->is_directory || info->is_symlink)
        return;

    size_t binary_goal = config->binary_handling == BINARY_INCLUDE ? 0 : BINARY_CHECK_SIZE;
    size_t prefix_goal = 0;

    if (!index->prefix_cache_disabled && info->size > 0 &&
        info->size <= index->prefix_file_limit && info->size <= MAX_FILE_SIZE &&
        index->prefix_used < index->prefix_budget)
    {
        size_t remaining = index->prefix_budget - index->prefix_used;
        prefix_goal = info->size;
        if (prefix_goal > remaining)
            prefix_goal = remaining;
    }

    size_t first_goal = binary_goal > 0 ? binary_goal : prefix_goal;
    if (first_goal > scratch_size)
        first_goal = scratch_size;
    if (first_goal == 0)
        return;

    FILE *file = fopen(full_path, "rb");
    if (!file)
        return;

    size_t read_count = fread(scratch, 1, first_goal, file);
    int read_error = ferror(file);
    int reached_eof = feof(file);

    if (read_count == 0 && info->size > 0)
    {
        fclose(file);
        return;
    }

    if (config->binary_handling != BINARY_INCLUDE)
        info->is_binary = buffer_has_nul(scratch, read_count < binary_goal ? read_count : binary_goal);

    if (info->is_binary || prefix_goal == 0 || read_count == 0)
    {
        fclose(file);
        return;
    }

    if (!read_error && !reached_eof && prefix_goal > read_count)
    {
        size_t want_more = prefix_goal - read_count;
        size_t more = fread(scratch + read_count, 1, want_more, file);
        read_count += more;
        read_error = ferror(file);
    }

    fclose(file);

    size_t cached_size = read_count < prefix_goal ? read_count : prefix_goal;
    char *owned_prefix = arena_alloc(&index->arena, cached_size, 1);
    if (!owned_prefix)
    {
        index->prefix_cache_disabled = 1;
        return;
    }

    memcpy(owned_prefix, scratch, cached_size);
    probe->data = owned_prefix;
    probe->size = cached_size;
    probe->complete = !read_error && cached_size == info->size;
    index->prefix_used += cached_size;
}

static size_t resolve_prefix_file_limit(void)
{
    const char *env = getenv("FCONCAT_PREFIX_FILE_KB");
    if (!env || env[0] == '\0')
        return FILE_INDEX_SMALL_CACHE_LIMIT;

    char *end = NULL;
    unsigned long long kb = strtoull(env, &end, 10);
    if (!end || *end != '\0')
        return FILE_INDEX_SMALL_CACHE_LIMIT;
    if (kb > (FILE_INDEX_PREFIX_LIMIT / 1024ULL))
        return FILE_INDEX_PREFIX_LIMIT;
    return (size_t)kb * 1024ULL;
}

FileIndex *file_index_create(size_t prefix_budget)
{
    FileIndex *index = calloc(1, sizeof(*index));
    if (!index)
        return NULL;

    index->capacity = FILE_INDEX_INITIAL_CAPACITY;
    index->entries = calloc(index->capacity, sizeof(*index->entries));
    if (!index->entries)
    {
        free(index);
        return NULL;
    }

    index->prefix_budget = prefix_budget;
    index->prefix_file_limit = resolve_prefix_file_limit();
    return index;
}

void file_index_destroy(FileIndex *index)
{
    if (!index)
        return;

    free(index->entries);
    arena_destroy(&index->arena);
    free(index);
}

int file_index_build(FileIndex *index,
                     FconcatContext *ctx,
                     const ResolvedConfig *config,
                     FilterEngine *filters,
                     int (*should_stop)(void *user_data),
                     void *user_data)
{
    if (!file_index_entries_are_valid(index) || !ctx || !config || !config->input_directory)
        return -1;

    char initial_full_path[MAX_PATH];
    if (build_full_path(initial_full_path, sizeof(initial_full_path), config->input_directory, "") != 0)
    {
        ctx->error(ctx, "Path too long: %s", config->input_directory);
        return -1;
    }

    struct stat initial_st;
    if (stat(initial_full_path, &initial_st) != 0)
    {
        ctx->error(ctx, "Cannot stat input directory: %s - %s", initial_full_path, strerror(errno));
        return -1;
    }

    if (!S_ISDIR(initial_st.st_mode))
    {
        ctx->error(ctx, "Input path is not a directory: %s", initial_full_path);
        return -1;
    }

    struct dirent **initial_entries = NULL;
    int initial_entry_count = 0;
    if (read_sorted_directory(initial_full_path, &initial_entries, &initial_entry_count) != 0)
    {
        int saved_errno = errno;
        ctx->warning(ctx, "Cannot open directory: %s - %s", initial_full_path, strerror(saved_errno));
        return -1;
    }

    DirStack *stack = dir_stack_create();
    if (!stack)
    {
        dir_entries_free(initial_entries, initial_entry_count);
        ctx->error(ctx, "Failed to allocate directory stack");
        return -1;
    }

    char *scratch = malloc(FILE_INDEX_PREFIX_LIMIT);
    if (!scratch)
    {
        dir_entries_free(initial_entries, initial_entry_count);
        dir_stack_destroy(stack);
        ctx->error(ctx, "Failed to allocate file probe buffer");
        return -1;
    }

    VisitedSet visited = {0};
    if (visited_set_add(&visited, initial_st.st_dev, initial_st.st_ino) != 0 ||
        dir_stack_push(stack, initial_full_path, "", initial_entries, initial_entry_count, 0, 1) != 0)
    {
        free(scratch);
        dir_entries_free(initial_entries, initial_entry_count);
        dir_stack_destroy(stack);
        return -1;
    }

    int result = 0;
    int skip_file_probe = config->output_file && strcmp(config->output_file, "/dev/null") == 0;
    int raw_null_fast = skip_file_probe && config->include_count == 0 && config->exclude_count == 0 &&
                        config->symlink_handling != SYMLINK_FOLLOW;
    while (stack->size > 0)
    {
        if (should_stop && should_stop(user_data))
        {
            result = -1;
            break;
        }

        DirStackEntry *current = dir_stack_peek(stack);
        if (current->next_entry >= current->entry_count)
        {
            if (current->visited_tracked)
                visited_set_pop(&visited);
            dir_stack_pop(stack);
            continue;
        }

        struct dirent *dirent = current->entries[current->next_entry++];

        if (current->level >= MAX_DIRECTORY_DEPTH)
        {
            ctx->warning(ctx, "Maximum directory depth (%d) exceeded, skipping deeper entries",
                         MAX_DIRECTORY_DEPTH);
            continue;
        }

#ifdef DT_UNKNOWN
        if (raw_null_fast && dirent->d_type != DT_UNKNOWN && dirent->d_type != DT_DIR)
        {
            if (dirent->d_type == DT_REG)
            {
                ProcessingStats *stats = (ProcessingStats *)ctx->stats;
                if (stats)
                {
                    fconcat_size_increment_saturated(&stats->processed_files);
                    fconcat_size_increment_saturated(&stats->total_files);
                }
            }
            continue;
        }

        if (raw_null_fast && dirent->d_type == DT_DIR)
        {
            char fast_dir_path[MAX_PATH];
            if (build_full_path(fast_dir_path, sizeof(fast_dir_path), current->path, dirent->d_name) != 0)
            {
                ctx->warning(ctx, "Path too long, skipping: %s", dirent->d_name);
                continue;
            }

            struct dirent **sub_entries = NULL;
            int sub_entry_count = 0;
            if (read_sorted_directory(fast_dir_path, &sub_entries, &sub_entry_count) != 0)
            {
                int saved_errno = errno;
                if (saved_errno == EACCES)
                    ctx->warning(ctx, "Permission denied accessing directory: %s", fast_dir_path);
                else
                    ctx->warning(ctx, "Cannot open directory: %s - %s", fast_dir_path, strerror(saved_errno));
                if (saved_errno == ENOMEM)
                {
                    result = -1;
                    break;
                }
                continue;
            }

            if (dir_stack_push(stack, fast_dir_path, "", sub_entries, sub_entry_count,
                               current->level + 1, 0) != 0)
            {
                dir_entries_free(sub_entries, sub_entry_count);
                ctx->warning(ctx, "Directory stack full, skipping: %s", fast_dir_path);
            }
            continue;
        }
#endif

        char entry_full_path[MAX_PATH];
        char entry_rel_path[MAX_PATH];
        if (build_full_path(entry_full_path, sizeof(entry_full_path), current->path, dirent->d_name) != 0)
        {
            ctx->warning(ctx, "Path too long, skipping: %s", dirent->d_name);
            continue;
        }
        if (build_relative_path(entry_rel_path, sizeof(entry_rel_path), current->relative_path, dirent->d_name) != 0)
        {
            ctx->warning(ctx, "Relative path too long, skipping: %s", dirent->d_name);
            continue;
        }

#ifdef DT_UNKNOWN
        if (skip_file_probe && config->symlink_handling != SYMLINK_FOLLOW &&
            dirent->d_type != DT_UNKNOWN && dirent->d_type != DT_LNK)
        {
            FileInfo fast_info = {0};
            fast_info.path = entry_rel_path;
            fast_info.is_directory = dirent->d_type == DT_DIR;
            fast_info.is_symlink = false;

            if (!fast_info.is_directory && dirent->d_type != DT_REG)
                continue;

            if (!raw_null_fast && !filter_engine_should_include_path(filters, ctx, entry_rel_path, &fast_info))
            {
                ctx->log(ctx, LOG_DEBUG, "Excluding path: %s", entry_rel_path);
                continue;
            }

            if (!fast_info.is_directory)
            {
                account_null_output_file(ctx, &fast_info);
                continue;
            }

            struct dirent **sub_entries = NULL;
            int sub_entry_count = 0;
            if (read_sorted_directory(entry_full_path, &sub_entries, &sub_entry_count) != 0)
            {
                int saved_errno = errno;
                if (saved_errno == EACCES)
                    ctx->warning(ctx, "Permission denied accessing directory: %s", entry_full_path);
                else
                    ctx->warning(ctx, "Cannot open directory: %s - %s", entry_full_path, strerror(saved_errno));
                if (saved_errno == ENOMEM)
                {
                    result = -1;
                    break;
                }
                continue;
            }

            if (dir_stack_push(stack, entry_full_path, entry_rel_path, sub_entries, sub_entry_count,
                               current->level + 1, 0) != 0)
            {
                dir_entries_free(sub_entries, sub_entry_count);
                ctx->warning(ctx, "Directory stack full, skipping: %s", entry_full_path);
            }
            continue;
        }
#endif

        struct stat st;
        if (lstat(entry_full_path, &st) != 0)
        {
            if (errno == EACCES)
                ctx->warning(ctx, "Permission denied accessing: %s", entry_full_path);
            else if (errno == ENOENT)
                ctx->warning(ctx, "File disappeared during indexing: %s", entry_full_path);
            else
                ctx->warning(ctx, "Cannot stat: %s - %s", entry_full_path, strerror(errno));
            continue;
        }

        FileInfo info = {0};
        info.path = entry_rel_path;
        info.size = stat_size_to_size_t(st.st_size);
        info.modified_time = st.st_mtime;
        info.is_directory = S_ISDIR(st.st_mode);
        info.is_symlink = S_ISLNK(st.st_mode);
        info.is_binary = false;
        info.permissions = (uint32_t)st.st_mode;

        char *resolved_path = NULL;
        if (info.is_symlink && config->symlink_handling == SYMLINK_FOLLOW)
        {
            resolved_path = realpath(entry_full_path, NULL);
            if (resolved_path)
            {
                struct stat resolved_st;
                if (stat(resolved_path, &resolved_st) == 0)
                {
                    info.is_directory = S_ISDIR(resolved_st.st_mode);
                    info.size = stat_size_to_size_t(resolved_st.st_size);
                    info.modified_time = resolved_st.st_mtime;
                    info.permissions = (uint32_t)resolved_st.st_mode;
                }
                else
                {
                    ctx->warning(ctx, "Cannot stat symlink target: %s", resolved_path);
                    free(resolved_path);
                    resolved_path = NULL;
                }
            }
            else
            {
                ctx->warning(ctx, "Cannot resolve symlink: %s - %s", entry_full_path, strerror(errno));
            }
        }

        if (!filter_engine_should_include_path(filters, ctx, entry_rel_path, &info))
        {
            ctx->log(ctx, LOG_DEBUG, "Excluding path: %s", entry_rel_path);
            free(resolved_path);
            continue;
        }

        PrefixProbe probe = {0};
        if (!skip_file_probe)
        {
            file_index_probe_file(index, config, entry_full_path, &info,
                                  scratch, FILE_INDEX_PREFIX_LIMIT, &probe);
        }

        if (!filter_engine_should_include_path(filters, ctx, entry_rel_path, &info))
        {
            ctx->log(ctx, LOG_DEBUG, "Excluding path: %s", entry_rel_path);
            free(resolved_path);
            continue;
        }

        if (skip_file_probe && !info.is_directory)
        {
            account_null_output_file(ctx, &info);
            free(resolved_path);
            continue;
        }

        if (!skip_file_probe)
        {
            FileIndexEntry *indexed = file_index_append(index, entry_full_path, entry_rel_path, current->level, &info);
            if (!indexed)
            {
                free(resolved_path);
                ctx->error(ctx, "Failed to allocate file index entry");
                result = -1;
                break;
            }

            indexed->prefix_data = probe.data;
            indexed->prefix_size = probe.size;
            indexed->prefix_complete = probe.complete;
        }

        if (info.is_directory)
        {
            const char *subdir_path = resolved_path ? resolved_path : entry_full_path;
            struct stat subdir_st;
            if (stat(subdir_path, &subdir_st) != 0)
            {
                ctx->warning(ctx, "Cannot stat subdirectory: %s", subdir_path);
                free(resolved_path);
                continue;
            }

            if (visited_set_contains(&visited, subdir_st.st_dev, subdir_st.st_ino))
            {
                ctx->warning(ctx, "Circular symlink detected, skipping: %s", subdir_path);
                free(resolved_path);
                continue;
            }

            if (visited_set_add(&visited, subdir_st.st_dev, subdir_st.st_ino) != 0)
            {
                ctx->warning(ctx, "Directory depth tracking full, skipping: %s", subdir_path);
                free(resolved_path);
                continue;
            }

            struct dirent **sub_entries = NULL;
            int sub_entry_count = 0;
            if (read_sorted_directory(subdir_path, &sub_entries, &sub_entry_count) != 0)
            {
                int saved_errno = errno;
                if (saved_errno == EACCES)
                    ctx->warning(ctx, "Permission denied accessing directory: %s", subdir_path);
                else
                    ctx->warning(ctx, "Cannot open directory: %s - %s", subdir_path, strerror(saved_errno));
                visited_set_pop(&visited);
                free(resolved_path);
                if (saved_errno == ENOMEM)
                {
                    result = -1;
                    break;
                }
                continue;
            }

            if (dir_stack_push(stack, subdir_path, entry_rel_path, sub_entries, sub_entry_count,
                               current->level + 1, 1) != 0)
            {
                dir_entries_free(sub_entries, sub_entry_count);
                visited_set_pop(&visited);
                ctx->warning(ctx, "Directory stack full, skipping: %s", subdir_path);
            }
        }

        free(resolved_path);
    }

    free(scratch);
    dir_stack_destroy(stack);
    return result;
}

size_t file_index_count(const FileIndex *index)
{
    return file_index_entries_are_valid(index) ? index->count : 0;
}

FileIndexEntry *file_index_entry(FileIndex *index, size_t offset)
{
    if (!file_index_entries_are_valid(index) || offset >= index->count)
        return NULL;
    return &index->entries[offset];
}

size_t file_index_prefix_used(const FileIndex *index)
{
    return index ? index->prefix_used : 0;
}

size_t file_index_prefix_budget(const FileIndex *index)
{
    return index ? index->prefix_budget : 0;
}
