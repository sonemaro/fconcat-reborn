#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define SMALL_BUFFER_SIZE 4096   // 4KB
#define MEDIUM_BUFFER_SIZE 16384 // 16KB
#define LARGE_BUFFER_SIZE 65536  // 64KB

#define SMALL_BUFFER_COUNT 20  // 20 small buffers
#define MEDIUM_BUFFER_COUNT 10 // 10 medium buffers
#define LARGE_BUFFER_COUNT 5   // 5 large buffers

#define TOTAL_POOL_SIZE (SMALL_BUFFER_COUNT + MEDIUM_BUFFER_COUNT + LARGE_BUFFER_COUNT)

BufferPool *buffer_pool_create(void)
{
    BufferPool *pool = calloc(1, sizeof(BufferPool));
    if (!pool)
        return NULL;

    pool->pool_size = TOTAL_POOL_SIZE;
    pool->small_buffer_count = SMALL_BUFFER_COUNT;
    pool->medium_buffer_count = MEDIUM_BUFFER_COUNT;
    pool->large_buffer_count = LARGE_BUFFER_COUNT;

    // Allocate arrays
    pool->buffers = malloc(pool->pool_size * sizeof(char *));
    pool->buffer_sizes = malloc(pool->pool_size * sizeof(size_t));
    pool->in_use = calloc(pool->pool_size, sizeof(bool));

    if (!pool->buffers || !pool->buffer_sizes || !pool->in_use)
    {
        free(pool->buffers);
        free(pool->buffer_sizes);
        free(pool->in_use);
        free(pool);
        return NULL;
    }

    int index = 0;

    // Create small buffers
    for (int i = 0; i < SMALL_BUFFER_COUNT; i++)
    {
        pool->buffers[index] = malloc(SMALL_BUFFER_SIZE);
        pool->buffer_sizes[index] = SMALL_BUFFER_SIZE;
        if (!pool->buffers[index])
        {
            // Cleanup on failure
            for (int j = 0; j < index; j++)
            {
                free(pool->buffers[j]);
            }
            free(pool->buffers);
            free(pool->buffer_sizes);
            free(pool->in_use);
            free(pool);
            return NULL;
        }
        index++;
    }

    // Create medium buffers
    for (int i = 0; i < MEDIUM_BUFFER_COUNT; i++)
    {
        pool->buffers[index] = malloc(MEDIUM_BUFFER_SIZE);
        pool->buffer_sizes[index] = MEDIUM_BUFFER_SIZE;
        if (!pool->buffers[index])
        {
            // Cleanup on failure
            for (int j = 0; j < index; j++)
            {
                free(pool->buffers[j]);
            }
            free(pool->buffers);
            free(pool->buffer_sizes);
            free(pool->in_use);
            free(pool);
            return NULL;
        }
        index++;
    }

    // Create large buffers
    for (int i = 0; i < LARGE_BUFFER_COUNT; i++)
    {
        pool->buffers[index] = malloc(LARGE_BUFFER_SIZE);
        pool->buffer_sizes[index] = LARGE_BUFFER_SIZE;
        if (!pool->buffers[index])
        {
            // Cleanup on failure
            for (int j = 0; j < index; j++)
            {
                free(pool->buffers[j]);
            }
            free(pool->buffers);
            free(pool->buffer_sizes);
            free(pool->in_use);
            free(pool);
            return NULL;
        }
        index++;
    }

    if (pthread_mutex_init(&pool->mutex, NULL) != 0)
    {
        // Cleanup on failure
        for (int i = 0; i < pool->pool_size; i++)
        {
            free(pool->buffers[i]);
        }
        free(pool->buffers);
        free(pool->buffer_sizes);
        free(pool->in_use);
        free(pool);
        return NULL;
    }

    return pool;
}

void buffer_pool_destroy(BufferPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->mutex);

    // Free all buffers
    for (int i = 0; i < pool->pool_size; i++)
    {
        free(pool->buffers[i]);
    }

    free(pool->buffers);
    free(pool->buffer_sizes);
    free(pool->in_use);

    pthread_mutex_unlock(&pool->mutex);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
}

char *buffer_pool_get(BufferPool *pool, size_t size)
{
    if (!pool)
        return malloc(size);

    pthread_mutex_lock(&pool->mutex);

    // Find best fit buffer
    int best_index = -1;
    size_t best_size = SIZE_MAX;

    for (int i = 0; i < pool->pool_size; i++)
    {
        if (!pool->in_use[i] && pool->buffer_sizes[i] >= size)
        {
            if (pool->buffer_sizes[i] < best_size)
            {
                best_size = pool->buffer_sizes[i];
                best_index = i;
            }
        }
    }

    if (best_index != -1)
    {
        pool->in_use[best_index] = true;
        pthread_mutex_unlock(&pool->mutex);
        return pool->buffers[best_index];
    }

    pthread_mutex_unlock(&pool->mutex);

    // Pool exhausted, fall back to malloc
    return malloc(size);
}

void buffer_pool_release(BufferPool *pool, char *buffer)
{
    if (!pool || !buffer)
    {
        // If no pool or buffer, assume it was malloc'd
        free(buffer);
        return;
    }

    pthread_mutex_lock(&pool->mutex);

    // Find buffer in pool
    for (int i = 0; i < pool->pool_size; i++)
    {
        if (pool->buffers[i] == buffer)
        {
            pool->in_use[i] = false;
            pthread_mutex_unlock(&pool->mutex);
            return;
        }
    }

    pthread_mutex_unlock(&pool->mutex);

    // Buffer not from pool, must be malloc'd
    free(buffer);
}

MemoryManager *memory_manager_create(void)
{
    MemoryManager *manager = calloc(1, sizeof(MemoryManager));
    if (!manager)
        return NULL;

    if (pthread_mutex_init(&manager->mutex, NULL) != 0)
    {
        free(manager);
        return NULL;
    }

    manager->buffer_pool = buffer_pool_create();
    if (!manager->buffer_pool)
    {
        pthread_mutex_destroy(&manager->mutex);
        free(manager);
        return NULL;
    }

    manager->track_allocations = 1;
    return manager;
}

void memory_manager_destroy(MemoryManager *manager)
{
    if (!manager)
        return;

    if (manager->buffer_pool)
    {
        buffer_pool_destroy(manager->buffer_pool);
    }

    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

void *memory_alloc(MemoryManager *manager, size_t size)
{
    void *ptr = malloc(size);
    if (!ptr)
        return NULL;

    if (manager && manager->track_allocations)
    {
        pthread_mutex_lock(&manager->mutex);
        manager->stats.total_allocated += size;
        manager->stats.current_usage += size;
        manager->stats.allocation_count++;
        if (manager->stats.current_usage > manager->stats.peak_usage)
        {
            manager->stats.peak_usage = manager->stats.current_usage;
        }
        pthread_mutex_unlock(&manager->mutex);
    }

    return ptr;
}

void *memory_realloc(MemoryManager *manager, void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0)
        return NULL;

    if (manager && manager->track_allocations)
    {
        pthread_mutex_lock(&manager->mutex);
        // Note: We can't track exact size changes without storing original sizes
        // This is a simplified implementation
        manager->stats.total_allocated += size;
        manager->stats.current_usage += size;
        manager->stats.allocation_count++;
        if (manager->stats.current_usage > manager->stats.peak_usage)
        {
            manager->stats.peak_usage = manager->stats.current_usage;
        }
        pthread_mutex_unlock(&manager->mutex);
    }

    return new_ptr;
}

void memory_free(MemoryManager *manager, void *ptr)
{
    if (!ptr)
        return;

    if (manager && manager->track_allocations)
    {
        pthread_mutex_lock(&manager->mutex);
        // Note: We can't track exact freed size without storing original sizes
        // This is a simplified implementation
        manager->stats.free_count++;
        pthread_mutex_unlock(&manager->mutex);
    }

    free(ptr);
}

MemoryStats memory_get_stats(MemoryManager *manager)
{
    MemoryStats stats = {0};
    if (!manager)
        return stats;

    pthread_mutex_lock(&manager->mutex);
    stats = manager->stats;
    pthread_mutex_unlock(&manager->mutex);

    return stats;
}

void memory_enable_tracking(MemoryManager *manager, int enable)
{
    if (!manager)
        return;

    pthread_mutex_lock(&manager->mutex);
    manager->track_allocations = enable;
    pthread_mutex_unlock(&manager->mutex);
}

char *memory_get_buffer(MemoryManager *manager, size_t size)
{
    if (!manager || !manager->buffer_pool)
    {
        return malloc(size);
    }

    char *buffer = buffer_pool_get(manager->buffer_pool, size);

    if (manager->track_allocations)
    {
        pthread_mutex_lock(&manager->mutex);
        manager->stats.allocation_count++;
        pthread_mutex_unlock(&manager->mutex);
    }

    return buffer;
}

void memory_release_buffer(MemoryManager *manager, char *buffer)
{
    if (!manager || !manager->buffer_pool)
    {
        free(buffer);
        return;
    }

    buffer_pool_release(manager->buffer_pool, buffer);

    if (manager->track_allocations)
    {
        pthread_mutex_lock(&manager->mutex);
        manager->stats.free_count++;
        pthread_mutex_unlock(&manager->mutex);
    }
}

// Stream buffer implementation
StreamBuffer *stream_buffer_create(MemoryManager *manager, size_t initial_size)
{
    StreamBuffer *buffer = memory_alloc(manager, sizeof(StreamBuffer));
    if (!buffer)
        return NULL;

    buffer->data = memory_alloc(manager, initial_size);
    if (!buffer->data)
    {
        memory_free(manager, buffer);
        return NULL;
    }

    buffer->size = 0;
    buffer->capacity = initial_size;
    buffer->position = 0;
    buffer->memory_manager = manager;

    return buffer;
}

int stream_buffer_write(StreamBuffer *buffer, const char *data, size_t size)
{
    if (!buffer || !data || size == 0)
        return -1;

    // Resize buffer if needed
    if (buffer->size + size > buffer->capacity)
    {
        size_t new_capacity = buffer->capacity * 2;
        while (new_capacity < buffer->size + size)
        {
            new_capacity *= 2;
        }

        char *new_data = memory_realloc(buffer->memory_manager, buffer->data, new_capacity);
        if (!new_data)
            return -1;

        buffer->data = new_data;
        buffer->capacity = new_capacity;
    }

    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;

    return 0;
}

int stream_buffer_flush(StreamBuffer *buffer)
{
    if (!buffer)
        return -1;

    // Write to stdout for now
    if (buffer->size > 0)
    {
        fwrite(buffer->data, 1, buffer->size, stdout);
        buffer->size = 0;
        buffer->position = 0;
    }

    return 0;
}

void stream_buffer_destroy(StreamBuffer *buffer)
{
    if (!buffer)
        return;

    memory_free(buffer->memory_manager, buffer->data);
    memory_free(buffer->memory_manager, buffer);
}
