#ifndef CORE_MEMORY_H
#define CORE_MEMORY_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        char **buffers;
        size_t *buffer_sizes;
        bool *in_use;
        int pool_size;
        int small_buffer_count;
        int medium_buffer_count;
        int large_buffer_count;
        pthread_mutex_t mutex;
    } BufferPool;

    // Memory statistics
    typedef struct
    {
        size_t total_allocated;
        size_t total_freed;
        size_t current_usage;
        size_t peak_usage;
        size_t allocation_count;
        size_t free_count;
    } MemoryStats;

    BufferPool *buffer_pool_create(void);
    void buffer_pool_destroy(BufferPool *pool);
    char *buffer_pool_get(BufferPool *pool, size_t size);
    void buffer_pool_release(BufferPool *pool, char *buffer);

    // Memory manager
    typedef struct
    {
        MemoryStats stats;
        BufferPool *buffer_pool;
        pthread_mutex_t mutex;
        int track_allocations;
    } MemoryManager;

    // Memory management functions
    MemoryManager *memory_manager_create(void);
    void memory_manager_destroy(MemoryManager *manager);
    void *memory_alloc(MemoryManager *manager, size_t size);
    void *memory_realloc(MemoryManager *manager, void *ptr, size_t size);
    void memory_free(MemoryManager *manager, void *ptr);
    MemoryStats memory_get_stats(MemoryManager *manager);
    void memory_enable_tracking(MemoryManager *manager, int enable);
    char *memory_get_buffer(MemoryManager *manager, size_t size);
    void memory_release_buffer(MemoryManager *manager, char *buffer);

    // Stream buffer functions
    typedef struct
    {
        char *data;
        size_t size;
        size_t capacity;
        size_t position;
        MemoryManager *memory_manager;
    } StreamBuffer;

    StreamBuffer *stream_buffer_create(MemoryManager *manager, size_t initial_size);
    int stream_buffer_write(StreamBuffer *buffer, const char *data, size_t size);
    int stream_buffer_flush(StreamBuffer *buffer);
    void stream_buffer_destroy(StreamBuffer *buffer);

#ifdef __cplusplus
}
#endif

#endif /* CORE_MEMORY_H */
