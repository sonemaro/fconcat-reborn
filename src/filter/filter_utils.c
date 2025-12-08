/**
 * @file filter_utils.c
 * @brief Shared utility functions for filter pattern matching
 */
#include "filter_utils.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fnmatch.h>

// FNM_CASEFOLD is a GNU extension, not available on macOS
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0
#define NEED_MANUAL_CASEFOLD 1
#endif

char *filter_str_to_lower(const char *str)
{
    if (!str)
        return NULL;
    
    size_t len = strlen(str);
    char *lower = malloc(len + 1);
    if (!lower)
        return NULL;
    
    for (size_t i = 0; i <= len; i++) {
        lower[i] = tolower((unsigned char)str[i]);
    }
    return lower;
}

const char *filter_get_basename(const char *path)
{
    if (!path)
        return NULL;

    const char *basename = strrchr(path, '/');
    const char *basename_win = strrchr(path, '\\');

    // Use whichever separator was found last
    if (basename_win > basename)
        basename = basename_win;

    return basename ? basename + 1 : path;
}

int filter_match_pattern(const char *pattern, const char *string)
{
    if (!pattern || !string)
        return 0;

    // Handle empty pattern
    if (strlen(pattern) == 0)
        return 0;

    // For simple patterns like "*.tsx", don't use FNM_PATHNAME
    // FNM_PATHNAME treats '/' specially which breaks simple wildcard matching
    if (fnmatch(pattern, string, 0) == 0)
        return 1;

    // Also try case-insensitive matching
#ifdef NEED_MANUAL_CASEFOLD
    // Manual case-insensitive matching for non-GNU systems (e.g., macOS)
    char *lower_pattern = filter_str_to_lower(pattern);
    char *lower_string = filter_str_to_lower(string);
    int result = 0;
    if (lower_pattern && lower_string) {
        result = (fnmatch(lower_pattern, lower_string, 0) == 0);
    }
    free(lower_pattern);
    free(lower_string);
    if (result)
        return 1;
#else
    if (fnmatch(pattern, string, FNM_CASEFOLD) == 0)
        return 1;
#endif

    return 0;
}

char *filter_normalize_pattern(const char *pattern)
{
    if (!pattern)
        return NULL;

    // Skip leading whitespace
    while (*pattern == ' ' || *pattern == '\t')
    {
        pattern++;
    }

    char *result = strdup(pattern);
    if (!result)
        return NULL;

    // Remove trailing whitespace
    size_t len = strlen(result);
    while (len > 0 && (result[len - 1] == ' ' || result[len - 1] == '\t'))
    {
        result[len - 1] = '\0';
        len--;
    }

    return result;
}
