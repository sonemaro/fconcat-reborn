#include "memory.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// Allocation header for accurate memory tracking
// Magic values are randomized at startup to prevent predictable exploitation
static uint32_t g_memory_magic = 0;
static uint32_t g_memory_freed_magic = 0;
static uint32_t g_memory_canary = 0;  // Tail canary for buffer overflow detection
static int g_magic_initialized = 0;

// Initialize magic values with random data (called once)
static void init_memory_magic(void)
{
    if (g_magic_initialized) return;
    
    // Try /dev/urandom first (cryptographically secure)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r1 = read(fd, &g_memory_magic, sizeof(g_memory_magic));
        ssize_t r2 = read(fd, &g_memory_freed_magic, sizeof(g_memory_freed_magic));
        ssize_t r3 = read(fd, &g_memory_canary, sizeof(g_memory_canary));
        close(fd);
        
        if (r1 == sizeof(g_memory_magic) && r2 == sizeof(g_memory_freed_magic) &&
            r3 == sizeof(g_memory_canary)) {
            // Ensure magic values are non-zero and different
            if (g_memory_magic == 0) g_memory_magic = 0xDEADBEEF;
            if (g_memory_freed_magic == 0) g_memory_freed_magic = 0xFEEDFACE;
            if (g_memory_canary == 0) g_memory_canary = 0xCAFEBABE;
            if (g_memory_magic == g_memory_freed_magic) g_memory_freed_magic ^= 0x12345678;
            if (g_memory_canary == g_memory_magic) g_memory_canary ^= 0x87654321;
            if (g_memory_canary == g_memory_freed_magic) g_memory_canary ^= 0xABCDEF00;
            g_magic_initialized = 1;
            return;
        }
    }
    
    // Fallback: use time-based seed + address randomization
    srandom((unsigned int)(time(NULL) ^ (uintptr_t)&g_memory_magic));
    g_memory_magic = (uint32_t)random() ^ 0xDEADBEEF;
    g_memory_freed_magic = (uint32_t)random() ^ 0xFEEDFACE;
    g_memory_canary = (uint32_t)random() ^ 0xCAFEBABE;
    
    // Ensure magic values are non-zero and different
    if (g_memory_magic == 0) g_memory_magic = 0xDEADBEEF;
    if (g_memory_freed_magic == 0) g_memory_freed_magic = 0xFEEDFACE;
    if (g_memory_canary == 0) g_memory_canary = 0xCAFEBABE;
    if (g_memory_magic == g_memory_freed_magic) g_memory_freed_magic ^= 0x12345678;
    if (g_memory_canary == g_memory_magic) g_memory_canary ^= 0x87654321;
    if (g_memory_canary == g_memory_freed_magic) g_memory_canary ^= 0xABCDEF00;
    
    g_magic_initialized = 1;
}

typedef struct {
    size_t size;       // Size of user allocation (not including header/canary)
    uint32_t magic;    // Magic number for corruption detection (header canary)
} AllocationHeader;

#define HEADER_SIZE (sizeof(AllocationHeader))
#define CANARY_SIZE (sizeof(uint32_t))  // Tail canary size

// Get user pointer from header
static inline void *header_to_user(AllocationHeader *header) {
    return (char *)header + HEADER_SIZE;
}

// Get header from user pointer
static inline AllocationHeader *user_to_header(void *ptr) {
    return (AllocationHeader *)((char *)ptr - HEADER_SIZE);
}

// Get tail canary pointer from header (placed after user data)
static inline uint32_t *get_tail_canary(AllocationHeader *header) {
    return (uint32_t *)((char *)header + HEADER_SIZE + header->size);
}

// Write tail canary value
static inline void write_tail_canary(AllocationHeader *header) {
    uint32_t *canary = get_tail_canary(header);
    *canary = g_memory_canary;
}

// Verify tail canary (returns 1 if valid, 0 if corrupted)
static inline int verify_tail_canary(AllocationHeader *header) {
    uint32_t *canary = get_tail_canary(header);
    return (*canary == g_memory_canary);
}

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
    // Ensure magic is initialized
    init_memory_magic();
    
    // Allocate with header + user data + tail canary
    AllocationHeader *header = (AllocationHeader *)malloc(HEADER_SIZE + size + CANARY_SIZE);
    if (!header)
        return NULL;

    header->size = size;
    header->magic = g_memory_magic;
    
    // Write tail canary after user data
    write_tail_canary(header);

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

    return header_to_user(header);
}

void *memory_realloc(MemoryManager *manager, void *ptr, size_t size)
{
    // Handle NULL ptr case (acts like malloc)
    if (!ptr)
        return memory_alloc(manager, size);
    
    // Handle size 0 case (acts like free)
    if (size == 0) {
        memory_free(manager, ptr);
        return NULL;
    }

    AllocationHeader *old_header = user_to_header(ptr);
    
    // Validate header magic number
    if (old_header->magic != g_memory_magic) {
        fprintf(stderr, "memory_realloc: header corruption detected or invalid pointer!\n");
        return NULL;
    }
    
    // Verify tail canary (buffer overflow detection)
    if (!verify_tail_canary(old_header)) {
        fprintf(stderr, "memory_realloc: buffer overflow detected (tail canary corrupted)!\n");
        return NULL;
    }
    
    size_t old_size = old_header->size;

    // Reallocate with header + new size + tail canary
    AllocationHeader *new_header = (AllocationHeader *)realloc(old_header, HEADER_SIZE + size + CANARY_SIZE);
    if (!new_header)
        return NULL;

    new_header->size = size;
    // header magic stays the same
    
    // Write new tail canary at new position
    write_tail_canary(new_header);

    if (manager && manager->track_allocations)
    {
        pthread_mutex_lock(&manager->mutex);
        // Correct tracking: subtract old size, add new size
        manager->stats.current_usage -= old_size;
        manager->stats.current_usage += size;
        manager->stats.total_allocated += size;  // Track total ever allocated
        manager->stats.allocation_count++;
        if (manager->stats.current_usage > manager->stats.peak_usage)
        {
            manager->stats.peak_usage = manager->stats.current_usage;
        }
        pthread_mutex_unlock(&manager->mutex);
    }

    return header_to_user(new_header);
}

void memory_free(MemoryManager *manager, void *ptr)
{
    if (!ptr)
        return;

    AllocationHeader *header = user_to_header(ptr);
    
    // Validate header magic number (double-free detection)
    if (header->magic == g_memory_freed_magic) {
        fprintf(stderr, "memory_free: double-free detected!\n");
        return;
    }
    if (header->magic != g_memory_magic) {
        fprintf(stderr, "memory_free: header corruption detected or invalid pointer!\n");
        return;
    }
    
    // Verify tail canary (buffer overflow detection)
    if (!verify_tail_canary(header)) {
        fprintf(stderr, "memory_free: buffer overflow detected (tail canary corrupted)!\n");
        // Still allow freeing to avoid leak, but warn loudly
    }
    
    size_t size = header->size;

    if (manager && manager->track_allocations)
    {
        pthread_mutex_lock(&manager->mutex);
        // Correct tracking: decrement usage by actual size
        manager->stats.current_usage -= size;
        manager->stats.total_freed += size;
        manager->stats.free_count++;
        pthread_mutex_unlock(&manager->mutex);
    }

    // Mark as freed before actually freeing (helps detect use-after-free in debug)
    header->magic = g_memory_freed_magic;
    free(header);
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

    // Check if addition would overflow
    if (buffer->size > SIZE_MAX - size)
        return -1;  // Would overflow

    // Resize buffer if needed
    if (buffer->size + size > buffer->capacity)
    {
        // Overflow-safe capacity doubling
        size_t new_capacity = buffer->capacity;
        size_t target = buffer->size + size;
        
        while (new_capacity < target) {
            // Check for overflow before doubling
            if (new_capacity > MAX_STREAM_BUFFER_SIZE / 2) {
                // Can't double, try exact fit if under limit
                if (target <= MAX_STREAM_BUFFER_SIZE) {
                    new_capacity = target;
                    break;
                }
                return -1;  // Would exceed max buffer size
            }
            new_capacity *= 2;
        }
        
        // Final limit check
        if (new_capacity > MAX_STREAM_BUFFER_SIZE)
            return -1;

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
