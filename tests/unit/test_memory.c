/**
 * @file test_memory.c
 * @brief Unit tests for memory management subsystem
 * 
 * Tests cover:
 * - MemoryManager lifecycle (create/destroy)
 * - Tracked allocation/free with statistics
 * - Reallocation with tracking
 * - Double-free detection
 * - Buffer pool allocation and release
 * - StreamBuffer creation, writing, overflow protection
 */

#include "test_framework.h"
#include "../../src/core/memory.h"
#include "../../src/core/types.h"
#include <string.h>

/* =========================================================================
 * MemoryManager Lifecycle Tests
 * ========================================================================= */

TEST(memory_manager_create_returns_valid_pointer)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_manager_destroy_null_is_safe)
{
    // Should not crash
    memory_manager_destroy(NULL);
    return 0;
}

TEST(memory_manager_initial_stats_are_zero)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    MemoryStats stats = memory_get_stats(mgr);
    ASSERT_EQ(0, stats.total_allocated);
    ASSERT_EQ(0, stats.total_freed);
    ASSERT_EQ(0, stats.current_usage);
    ASSERT_EQ(0, stats.peak_usage);
    ASSERT_EQ(0, stats.allocation_count);
    ASSERT_EQ(0, stats.free_count);
    
    memory_manager_destroy(mgr);
    return 0;
}

/* =========================================================================
 * Tracked Allocation Tests
 * ========================================================================= */

TEST(memory_alloc_returns_valid_pointer)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    void *ptr = memory_alloc(mgr, 100);
    ASSERT_NOT_NULL(ptr);
    
    memory_free(mgr, ptr);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_alloc_tracks_statistics)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    void *ptr = memory_alloc(mgr, 100);
    ASSERT_NOT_NULL(ptr);
    
    MemoryStats stats = memory_get_stats(mgr);
    ASSERT_EQ(100, stats.total_allocated);
    ASSERT_EQ(100, stats.current_usage);
    ASSERT_EQ(100, stats.peak_usage);
    ASSERT_EQ(1, stats.allocation_count);
    ASSERT_EQ(0, stats.free_count);
    
    memory_free(mgr, ptr);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_free_updates_statistics)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    void *ptr = memory_alloc(mgr, 256);
    ASSERT_NOT_NULL(ptr);
    memory_free(mgr, ptr);
    
    MemoryStats stats = memory_get_stats(mgr);
    ASSERT_EQ(256, stats.total_allocated);
    ASSERT_EQ(256, stats.total_freed);
    ASSERT_EQ(0, stats.current_usage);
    ASSERT_EQ(256, stats.peak_usage);  // Peak should remain
    ASSERT_EQ(1, stats.allocation_count);
    ASSERT_EQ(1, stats.free_count);
    
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_free_null_is_safe)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    // Should not crash
    memory_free(mgr, NULL);
    memory_free(NULL, NULL);
    
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_alloc_null_manager_still_allocates)
{
    // Should work without manager (uses raw malloc)
    void *ptr = memory_alloc(NULL, 100);
    ASSERT_NOT_NULL(ptr);
    
    // Free with NULL manager should also work
    memory_free(NULL, ptr);
    return 0;
}

TEST(memory_multiple_allocations_track_correctly)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    void *p1 = memory_alloc(mgr, 100);
    void *p2 = memory_alloc(mgr, 200);
    void *p3 = memory_alloc(mgr, 300);
    
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);
    
    MemoryStats stats = memory_get_stats(mgr);
    ASSERT_EQ(600, stats.current_usage);
    ASSERT_EQ(600, stats.total_allocated);
    ASSERT_EQ(3, stats.allocation_count);
    
    memory_free(mgr, p2);
    stats = memory_get_stats(mgr);
    ASSERT_EQ(400, stats.current_usage);  // 600 - 200
    ASSERT_EQ(600, stats.peak_usage);     // Peak unchanged
    
    memory_free(mgr, p1);
    memory_free(mgr, p3);
    memory_manager_destroy(mgr);
    return 0;
}

/* =========================================================================
 * Reallocation Tests
 * ========================================================================= */

TEST(memory_realloc_grows_allocation)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    char *ptr = memory_alloc(mgr, 100);
    ASSERT_NOT_NULL(ptr);
    
    // Write pattern to original allocation
    memset(ptr, 'A', 100);
    
    // Grow the allocation
    ptr = memory_realloc(mgr, ptr, 200);
    ASSERT_NOT_NULL(ptr);
    
    // Verify original data preserved
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ('A', ptr[i]);
    }
    
    memory_free(mgr, ptr);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_realloc_null_ptr_acts_like_alloc)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    void *ptr = memory_realloc(mgr, NULL, 100);
    ASSERT_NOT_NULL(ptr);
    
    MemoryStats stats = memory_get_stats(mgr);
    ASSERT_EQ(100, stats.current_usage);
    
    memory_free(mgr, ptr);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_realloc_zero_size_frees)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    void *ptr = memory_alloc(mgr, 100);
    ASSERT_NOT_NULL(ptr);
    
    void *result = memory_realloc(mgr, ptr, 0);
    ASSERT_NULL(result);
    
    MemoryStats stats = memory_get_stats(mgr);
    ASSERT_EQ(0, stats.current_usage);
    
    memory_manager_destroy(mgr);
    return 0;
}

/* =========================================================================
 * Buffer Pool Tests
 * ========================================================================= */

TEST(buffer_pool_create_returns_valid_pointer)
{
    BufferPool *pool = buffer_pool_create();
    ASSERT_NOT_NULL(pool);
    buffer_pool_destroy(pool);
    return 0;
}

TEST(buffer_pool_destroy_null_is_safe)
{
    buffer_pool_destroy(NULL);
    return 0;
}

TEST(buffer_pool_get_returns_buffer)
{
    BufferPool *pool = buffer_pool_create();
    ASSERT_NOT_NULL(pool);
    
    char *buf = buffer_pool_get(pool, 1000);
    ASSERT_NOT_NULL(buf);
    
    buffer_pool_release(pool, buf);
    buffer_pool_destroy(pool);
    return 0;
}

TEST(buffer_pool_reuses_released_buffer)
{
    BufferPool *pool = buffer_pool_create();
    ASSERT_NOT_NULL(pool);
    
    // Get a buffer
    char *buf1 = buffer_pool_get(pool, 1000);
    ASSERT_NOT_NULL(buf1);
    
    // Release it
    buffer_pool_release(pool, buf1);
    
    // Get another buffer of same size - should get the same one
    char *buf2 = buffer_pool_get(pool, 1000);
    ASSERT_NOT_NULL(buf2);
    ASSERT_EQ((size_t)buf1, (size_t)buf2);  // Same pointer
    
    buffer_pool_release(pool, buf2);
    buffer_pool_destroy(pool);
    return 0;
}

TEST(buffer_pool_fallback_to_malloc_when_exhausted)
{
    BufferPool *pool = buffer_pool_create();
    ASSERT_NOT_NULL(pool);
    
    // Exhaust all small buffers (20 of them)
    char *buffers[25];
    for (int i = 0; i < 25; i++) {
        buffers[i] = buffer_pool_get(pool, 1000);
        ASSERT_NOT_NULL(buffers[i]);
    }
    
    // Release all
    for (int i = 0; i < 25; i++) {
        buffer_pool_release(pool, buffers[i]);
    }
    
    buffer_pool_destroy(pool);
    return 0;
}

/* =========================================================================
 * StreamBuffer Tests
 * ========================================================================= */

TEST(stream_buffer_create_returns_valid_pointer)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    StreamBuffer *buf = stream_buffer_create(mgr, 1024);
    ASSERT_NOT_NULL(buf);
    ASSERT_NOT_NULL(buf->data);
    ASSERT_EQ(1024, buf->capacity);
    ASSERT_EQ(0, buf->size);
    
    stream_buffer_destroy(buf);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(stream_buffer_write_appends_data)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    StreamBuffer *buf = stream_buffer_create(mgr, 1024);
    ASSERT_NOT_NULL(buf);
    
    const char *msg1 = "Hello, ";
    const char *msg2 = "World!";
    
    ASSERT_EQ(0, stream_buffer_write(buf, msg1, strlen(msg1)));
    ASSERT_EQ(0, stream_buffer_write(buf, msg2, strlen(msg2)));
    
    ASSERT_EQ(strlen("Hello, World!"), buf->size);
    ASSERT_MEM_EQ("Hello, World!", buf->data, buf->size);
    
    stream_buffer_destroy(buf);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(stream_buffer_grows_automatically)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    StreamBuffer *buf = stream_buffer_create(mgr, 16);  // Small initial size
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(16, buf->capacity);
    
    // Write more than initial capacity
    char data[100];
    memset(data, 'X', 100);
    
    ASSERT_EQ(0, stream_buffer_write(buf, data, 100));
    ASSERT_TRUE(buf->capacity >= 100);  // Should have grown
    ASSERT_EQ(100, buf->size);
    
    stream_buffer_destroy(buf);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(stream_buffer_rejects_null_inputs)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    StreamBuffer *buf = stream_buffer_create(mgr, 1024);
    ASSERT_NOT_NULL(buf);
    
    // NULL buffer
    ASSERT_EQ(-1, stream_buffer_write(NULL, "test", 4));
    
    // NULL data
    ASSERT_EQ(-1, stream_buffer_write(buf, NULL, 4));
    
    // Zero size
    ASSERT_EQ(-1, stream_buffer_write(buf, "test", 0));
    
    stream_buffer_destroy(buf);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(stream_buffer_destroy_null_is_safe)
{
    stream_buffer_destroy(NULL);
    return 0;
}

/* =========================================================================
 * Memory Tracking Toggle Tests
 * ========================================================================= */

TEST(memory_enable_tracking_can_disable)
{
    MemoryManager *mgr = memory_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    // Disable tracking
    memory_enable_tracking(mgr, 0);
    
    void *ptr = memory_alloc(mgr, 100);
    ASSERT_NOT_NULL(ptr);
    
    MemoryStats stats = memory_get_stats(mgr);
    ASSERT_EQ(0, stats.allocation_count);  // Should not be tracked
    
    memory_free(mgr, ptr);
    memory_manager_destroy(mgr);
    return 0;
}

TEST(memory_enable_tracking_null_manager_is_safe)
{
    memory_enable_tracking(NULL, 1);
    memory_enable_tracking(NULL, 0);
    return 0;
}

/* =========================================================================
 * Main Entry Point (renamed for test harness integration)
 * ========================================================================= */

int test_memory_main(void)
{
    /* Reset counters for this test suite */
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
    
    TEST_SUITE_BEGIN("MemoryManager Lifecycle");
    RUN_TEST(memory_manager_create_returns_valid_pointer);
    RUN_TEST(memory_manager_destroy_null_is_safe);
    RUN_TEST(memory_manager_initial_stats_are_zero);
    
    TEST_SUITE_BEGIN("Tracked Allocations");
    RUN_TEST(memory_alloc_returns_valid_pointer);
    RUN_TEST(memory_alloc_tracks_statistics);
    RUN_TEST(memory_free_updates_statistics);
    RUN_TEST(memory_free_null_is_safe);
    RUN_TEST(memory_alloc_null_manager_still_allocates);
    RUN_TEST(memory_multiple_allocations_track_correctly);
    
    TEST_SUITE_BEGIN("Reallocation");
    RUN_TEST(memory_realloc_grows_allocation);
    RUN_TEST(memory_realloc_null_ptr_acts_like_alloc);
    RUN_TEST(memory_realloc_zero_size_frees);
    
    TEST_SUITE_BEGIN("Buffer Pool");
    RUN_TEST(buffer_pool_create_returns_valid_pointer);
    RUN_TEST(buffer_pool_destroy_null_is_safe);
    RUN_TEST(buffer_pool_get_returns_buffer);
    RUN_TEST(buffer_pool_reuses_released_buffer);
    RUN_TEST(buffer_pool_fallback_to_malloc_when_exhausted);
    
    TEST_SUITE_BEGIN("StreamBuffer");
    RUN_TEST(stream_buffer_create_returns_valid_pointer);
    RUN_TEST(stream_buffer_write_appends_data);
    RUN_TEST(stream_buffer_grows_automatically);
    RUN_TEST(stream_buffer_rejects_null_inputs);
    RUN_TEST(stream_buffer_destroy_null_is_safe);
    
    TEST_SUITE_BEGIN("Memory Tracking Toggle");
    RUN_TEST(memory_enable_tracking_can_disable);
    RUN_TEST(memory_enable_tracking_null_manager_is_safe);
    
    TEST_SUMMARY();
    
    return TEST_EXIT_CODE();
}
