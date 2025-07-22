
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

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
    for (int i = 0; i < manager->error_count; i++)
    {
        free(manager->errors[i].message);
        free(manager->errors[i].file);
        free(manager->errors[i].function);
    }

    pthread_mutex_unlock(&manager->mutex);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

void error_report_context(ErrorManager *manager, FconcatErrorCode code, const char *file, int line, const char *function, const char *format, ...)
{
    if (!manager || manager->error_count >= 1000)
        return;

    pthread_mutex_lock(&manager->mutex);

    ErrorContext *ctx = &manager->errors[manager->error_count];
    ctx->code = code;
    ctx->file = strdup(file);
    ctx->line = line;
    ctx->function = strdup(function);
    ctx->timestamp = time(NULL);

    // Format message
    va_list args;
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    ctx->message = malloc(len + 1);
    if (ctx->message)
    {
        va_start(args, format);
        vsnprintf(ctx->message, len + 1, format, args);
        va_end(args);
    }

    manager->error_count++;

    // Also print to stderr
    fprintf(stderr, "[ERROR] %s:%d in %s(): %s\n", file, line, function, ctx->message);

    pthread_mutex_unlock(&manager->mutex);
}

void error_report(ErrorManager *manager, FconcatErrorCode code, const char *format, ...)
{
    if (!manager || manager->error_count >= 1000)
        return;

    pthread_mutex_lock(&manager->mutex);

    ErrorContext *ctx = &manager->errors[manager->error_count];
    ctx->code = code;
    ctx->file = strdup("unknown");
    ctx->line = 0;
    ctx->function = strdup("unknown");
    ctx->timestamp = time(NULL);

    // Format message
    va_list args;
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    ctx->message = malloc(len + 1);
    if (ctx->message)
    {
        va_start(args, format);
        vsnprintf(ctx->message, len + 1, format, args);
        va_end(args);
    }

    manager->error_count++;

    // Also print to stderr
    fprintf(stderr, "[ERROR] %s\n", ctx->message);

    pthread_mutex_unlock(&manager->mutex);
}

void warning_report(ErrorManager *manager, const char *format, ...)
{
    if (!manager)
        return;

    pthread_mutex_lock(&manager->mutex);
    manager->warning_count++;
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
    int count = manager->error_count;
    pthread_mutex_unlock(&manager->mutex);
    return count;
}

int warning_get_count(ErrorManager *manager)
{
    if (!manager)
        return 0;
    pthread_mutex_lock(&manager->mutex);
    int count = manager->warning_count;
    pthread_mutex_unlock(&manager->mutex);
    return count;
}

void error_clear(ErrorManager *manager)
{
    if (!manager)
        return;

    pthread_mutex_lock(&manager->mutex);

    // Free all error messages
    for (int i = 0; i < manager->error_count; i++)
    {
        free(manager->errors[i].message);
        free(manager->errors[i].file);
        free(manager->errors[i].function);
    }

    manager->error_count = 0;
    manager->warning_count = 0;

    pthread_mutex_unlock(&manager->mutex);
}
