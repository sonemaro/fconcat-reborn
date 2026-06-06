
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

static int error_count_clamped(int count)
{
    if (count < 0)
        return 0;
    if (count > MAX_ERRORS)
        return MAX_ERRORS;
    return count;
}

static void error_context_cleanup(ErrorContext *ctx);

static int error_cleanup_count(const ErrorManager *manager)
{
    if (!manager)
        return 0;
    if (manager->error_count < 0 || manager->error_count > MAX_ERRORS)
        return MAX_ERRORS;
    return manager->error_count;
}

static void error_cleanup_tracked_slots(ErrorManager *manager)
{
    int error_count = error_cleanup_count(manager);
    for (int i = 0; i < error_count; i++)
        error_context_cleanup(&manager->errors[i]);
}

static int error_prepare_append_locked(ErrorManager *manager)
{
    if (!manager)
        return -1;
    if (manager->error_count < 0)
    {
        error_cleanup_tracked_slots(manager);
        manager->error_count = 0;
    }
    if (manager->error_count >= MAX_ERRORS)
    {
        manager->error_count = MAX_ERRORS;
        return -1;
    }
    return 0;
}

static void warning_count_increment_locked(ErrorManager *manager)
{
    if (!manager)
        return;
    if (manager->warning_count < 0)
        manager->warning_count = 0;
    if (manager->warning_count < INT_MAX)
        manager->warning_count++;
}

static void error_context_cleanup(ErrorContext *ctx)
{
    if (!ctx)
        return;
    free(ctx->message);
    free(ctx->file);
    free(ctx->function);
    memset(ctx, 0, sizeof(*ctx));
}

ErrorManager *error_manager_create(void)
{
    ErrorManager *manager = calloc(1, sizeof(ErrorManager));
    if (!manager)
        return NULL;

    if (pthread_mutex_init(&manager->mutex, NULL) != 0)
    {
        free(manager);
        return NULL;
    }

    return manager;
}

void error_manager_destroy(ErrorManager *manager)
{
    if (!manager)
        return;

    pthread_mutex_lock(&manager->mutex);

    // Free all error messages
    error_cleanup_tracked_slots(manager);

    pthread_mutex_unlock(&manager->mutex);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

void error_report_context(ErrorManager *manager, FconcatErrorCode code, const char *file, int line, const char *function, const char *format, ...)
{
    if (!manager || !format)
        return;

    pthread_mutex_lock(&manager->mutex);
    if (error_prepare_append_locked(manager) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return;
    }

    ErrorContext *ctx = &manager->errors[manager->error_count];
    ctx->code = code;
    ctx->file = file ? strdup(file) : NULL;
    ctx->line = line;
    ctx->function = function ? strdup(function) : NULL;
    ctx->timestamp = time(NULL);

    // Check if strdup failed (out of memory) - still record error but with NULL fields
    // This is acceptable degradation since we're already in an error path

    // Format message
    va_list args;
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (len < 0) {
        ctx->message = NULL;
    } else {
        ctx->message = malloc((size_t)len + 1);
        if (ctx->message)
        {
            va_start(args, format);
            vsnprintf(ctx->message, (size_t)len + 1, format, args);
            va_end(args);
        }
    }

    manager->error_count++;

    // Also print to stderr (handle NULL fields gracefully)
    fprintf(stderr, "[ERROR] %s:%d in %s(): %s\n", 
            file ? file : "unknown", 
            line, 
            function ? function : "unknown", 
            ctx->message ? ctx->message : "(out of memory)");

    pthread_mutex_unlock(&manager->mutex);
}

void error_report(ErrorManager *manager, FconcatErrorCode code, const char *format, ...)
{
    if (!manager || !format)
        return;

    pthread_mutex_lock(&manager->mutex);
    if (error_prepare_append_locked(manager) != 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return;
    }

    ErrorContext *ctx = &manager->errors[manager->error_count];
    ctx->code = code;
    ctx->file = strdup("unknown");      // May be NULL on OOM - acceptable degradation
    ctx->line = 0;
    ctx->function = strdup("unknown");  // May be NULL on OOM - acceptable degradation
    ctx->timestamp = time(NULL);

    // Format message
    va_list args;
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (len < 0) {
        ctx->message = NULL;
    } else {
        ctx->message = malloc((size_t)len + 1);
        if (ctx->message)
        {
            va_start(args, format);
            vsnprintf(ctx->message, (size_t)len + 1, format, args);
            va_end(args);
        }
    }

    manager->error_count++;

    // Also print to stderr (handle NULL message)
    fprintf(stderr, "[ERROR] %s\n", ctx->message ? ctx->message : "(out of memory)");

    pthread_mutex_unlock(&manager->mutex);
}

void warning_report(ErrorManager *manager, const char *format, ...)
{
    if (!manager || !format)
        return;

    pthread_mutex_lock(&manager->mutex);
    warning_count_increment_locked(manager);
    pthread_mutex_unlock(&manager->mutex);

    // Print to stderr
    fprintf(stderr, "[WARNING] ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

int error_get_count(ErrorManager *manager)
{
    if (!manager)
        return 0;
    pthread_mutex_lock(&manager->mutex);
    int count = error_count_clamped(manager->error_count);
    pthread_mutex_unlock(&manager->mutex);
    return count;
}

int warning_get_count(ErrorManager *manager)
{
    if (!manager)
        return 0;
    pthread_mutex_lock(&manager->mutex);
    int count = manager->warning_count < 0 ? 0 : manager->warning_count;
    pthread_mutex_unlock(&manager->mutex);
    return count;
}

void error_clear(ErrorManager *manager)
{
    if (!manager)
        return;

    pthread_mutex_lock(&manager->mutex);

    // Free all error messages
    error_cleanup_tracked_slots(manager);

    manager->error_count = 0;
    manager->warning_count = 0;

    pthread_mutex_unlock(&manager->mutex);
}
