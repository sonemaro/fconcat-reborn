#ifdef FCONCAT_LEAK_GUARD

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef LEAK_GUARD_MAX_ALLOCATIONS
#define LEAK_GUARD_MAX_ALLOCATIONS 1000000
#endif

typedef struct
{
    void *ptr;
    size_t size;
    unsigned char live;
} LeakEntry;

static LeakEntry g_entries[LEAK_GUARD_MAX_ALLOCATIONS];
static size_t g_entry_count = 0;
static size_t g_live_count = 0;
static size_t g_live_bytes = 0;
static size_t g_peak_live_bytes = 0;
static size_t g_overflow_count = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_disabled = 0;

void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);
char *__real_realpath(const char *path, char *resolved_path);

static void leak_guard_track_alloc(void *ptr, size_t size)
{
    if (!ptr || g_disabled)
        return;

    pthread_mutex_lock(&g_lock);
    if (g_entry_count < LEAK_GUARD_MAX_ALLOCATIONS)
    {
        g_entries[g_entry_count].ptr = ptr;
        g_entries[g_entry_count].size = size;
        g_entries[g_entry_count].live = 1;
        g_entry_count++;
        g_live_count++;
        g_live_bytes += size;
        if (g_live_bytes > g_peak_live_bytes)
            g_peak_live_bytes = g_live_bytes;
    }
    else
    {
        g_overflow_count++;
    }
    pthread_mutex_unlock(&g_lock);
}

static size_t leak_guard_untrack(void *ptr)
{
    if (!ptr || g_disabled)
        return 0;

    size_t size = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = g_entry_count; i > 0; i--)
    {
        LeakEntry *entry = &g_entries[i - 1];
        if (entry->live && entry->ptr == ptr)
        {
            entry->live = 0;
            size = entry->size;
            g_live_count--;
            g_live_bytes -= size;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return size;
}

static void leak_guard_resize(void *old_ptr, void *new_ptr, size_t new_size)
{
    if (g_disabled)
        return;

    pthread_mutex_lock(&g_lock);
    for (size_t i = g_entry_count; i > 0; i--)
    {
        LeakEntry *entry = &g_entries[i - 1];
        if (entry->live && entry->ptr == old_ptr)
        {
            g_live_bytes -= entry->size;
            entry->ptr = new_ptr;
            entry->size = new_size;
            g_live_bytes += new_size;
            if (g_live_bytes > g_peak_live_bytes)
                g_peak_live_bytes = g_live_bytes;
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    pthread_mutex_unlock(&g_lock);

    leak_guard_track_alloc(new_ptr, new_size);
}

void *__wrap_malloc(size_t size)
{
    void *ptr = __real_malloc(size);
    leak_guard_track_alloc(ptr, size);
    return ptr;
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    void *ptr = __real_calloc(nmemb, size);
    if (nmemb != 0 && size > SIZE_MAX / nmemb)
        leak_guard_track_alloc(ptr, 0);
    else
        leak_guard_track_alloc(ptr, nmemb * size);
    return ptr;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return __wrap_malloc(size);

    if (size == 0)
    {
        leak_guard_untrack(ptr);
        __real_free(ptr);
        return NULL;
    }

    void *new_ptr = __real_realloc(ptr, size);
    if (new_ptr)
        leak_guard_resize(ptr, new_ptr, size);
    return new_ptr;
}

void __wrap_free(void *ptr)
{
    leak_guard_untrack(ptr);
    __real_free(ptr);
}

char *__wrap_strdup(const char *s)
{
    if (!s)
    {
        errno = EINVAL;
        return NULL;
    }

    size_t len = strlen(s) + 1;
    char *copy = __real_malloc(len);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    leak_guard_track_alloc(copy, len);
    return copy;
}

char *__wrap_realpath(const char *path, char *resolved_path)
{
    if (resolved_path)
        return __real_realpath(path, resolved_path);

    char *buffer = __real_malloc(PATH_MAX);
    if (!buffer)
        return NULL;

    if (!__real_realpath(path, buffer))
    {
        __real_free(buffer);
        return NULL;
    }

    leak_guard_track_alloc(buffer, strlen(buffer) + 1);
    return buffer;
}

static void leak_guard_report(void)
{
    g_disabled = 1;
    pthread_mutex_lock(&g_lock);
    int failed = (g_live_count != 0 || g_overflow_count != 0);
    if (failed)
    {
        fprintf(stderr,
                "\nLEAK_GUARD: FAILED live_allocations=%zu live_bytes=%zu peak_live_bytes=%zu overflow=%zu\n",
                g_live_count, g_live_bytes, g_peak_live_bytes, g_overflow_count);
        size_t printed = 0;
        for (size_t i = 0; i < g_entry_count && printed < 32; i++)
        {
            if (g_entries[i].live)
            {
                fprintf(stderr, "LEAK_GUARD: live ptr=%p size=%zu\n",
                        g_entries[i].ptr, g_entries[i].size);
                printed++;
            }
        }
    }
    else
    {
        fprintf(stderr,
                "LEAK_GUARD: OK live_allocations=0 peak_live_bytes=%zu tracked_allocations=%zu\n",
                g_peak_live_bytes, g_entry_count);
    }
    pthread_mutex_unlock(&g_lock);
    g_disabled = 0;

    if (failed)
        _Exit(23);
}

__attribute__((constructor)) static void leak_guard_init(void)
{
    atexit(leak_guard_report);
}

#endif /* FCONCAT_LEAK_GUARD */
