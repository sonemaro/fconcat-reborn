#include "filter.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// Symlink handling context
typedef struct
{
    SymlinkHandling handling;
} SymlinkContext;

static int symlink_match_path(const char *path, FileInfo *info, void *context)
{
    (void)path;
    SymlinkContext *ctx = (SymlinkContext *)context;
    if (!ctx || !info)
        return 0;

    return info->is_symlink;
}

// FIXED: Enhanced symlink transformation for placeholder mode
static int symlink_transform(const char *path, const char *input, size_t input_size, char **output, size_t *output_size, void *context)
{
    SymlinkContext *ctx = (SymlinkContext *)context;
    (void)input;      // Mark as intentionally unused
    (void)input_size; // Mark as intentionally unused
    if (!ctx || !output || !output_size)
        return -1;

    if (ctx->handling == SYMLINK_PLACEHOLDER)
    {
        // Read the symlink target - FIXED: Use larger buffer to avoid truncation
        char target[2048];
        ssize_t len = readlink(path, target, sizeof(target) - 1);

        char placeholder[2048]; // FIXED: Larger buffer
        if (len > 0)
        {
            target[len] = '\0';
            // FIXED: Safe string formatting
            int ret = snprintf(placeholder, sizeof(placeholder), "// [Symbolic link to: %s]\n", target);
            if (ret < 0 || ret >= (int)sizeof(placeholder))
            {
                // Fallback if target is too long
                strncpy(placeholder, "// [Symbolic link - target too long]\n", sizeof(placeholder) - 1);
                placeholder[sizeof(placeholder) - 1] = '\0';
            }
        }
        else
        {
            strncpy(placeholder, "// [Symbolic link - target unreadable]\n", sizeof(placeholder) - 1);
            placeholder[sizeof(placeholder) - 1] = '\0';
        }

        size_t placeholder_len = strlen(placeholder);
        *output = malloc(placeholder_len);
        if (!*output)
            return -1;

        memcpy(*output, placeholder, placeholder_len);
        *output_size = placeholder_len;
        return 0;
    }

    return -1; // No transformation
}

static void destroy_symlink_context(void *context)
{
    free(context);
}

int filter_symlink_handling_init_internal(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    SymlinkContext *ctx = malloc(sizeof(SymlinkContext));
    if (!ctx)
    {
        return -1;
    }

    ctx->handling = config->symlink_handling;

    if (config->symlink_handling == SYMLINK_SKIP)
    {
        // Create exclude rule for symlinks
        FilterRule rule = {
            .type = FILTER_TYPE_EXCLUDE,
            .priority = 80,
            .match_path = symlink_match_path,
            .match_content = NULL,
            .transform = NULL,
            .destroy_context = destroy_symlink_context,
            .context = ctx};

        return filter_engine_add_rule_internal(engine, &rule);
    }
    else if (config->symlink_handling == SYMLINK_PLACEHOLDER)
    {
        // Create transform rule for symlinks
        FilterRule rule = {
            .type = FILTER_TYPE_TRANSFORM,
            .priority = 80,
            .match_path = symlink_match_path,
            .match_content = NULL,
            .transform = symlink_transform,
            .destroy_context = destroy_symlink_context,
            .context = ctx};

        return filter_engine_add_rule_internal(engine, &rule);
    }
    else if (config->symlink_handling == SYMLINK_FOLLOW)
    {
        // Symlink following is handled in directory traversal
        free(ctx);
        return 0;
    }
    else if (config->symlink_handling == SYMLINK_INCLUDE)
    {
        // Include symlinks as-is (no filtering)
        free(ctx);
        return 0;
    }

    free(ctx);
    return 0;
}

int filter_symlink_handling_init(FilterEngine *engine, const ResolvedConfig *config)
{
    if (!engine || !config)
        return -1;

    pthread_mutex_lock(&engine->mutex);
    int result = filter_symlink_handling_init_internal(engine, config);
    pthread_mutex_unlock(&engine->mutex);
    return result;
}