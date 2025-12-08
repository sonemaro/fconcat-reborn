/**
 * @file filter_utils.h
 * @brief Shared utility functions for filter pattern matching
 * 
 * This header provides common pattern matching utilities used by both
 * include and exclude filters to avoid code duplication.
 */
#ifndef FILTER_UTILS_H
#define FILTER_UTILS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert a string to lowercase (for case-insensitive matching)
 * 
 * @param str The string to convert
 * @return Newly allocated lowercase string, or NULL on failure. Caller must free().
 */
char *filter_str_to_lower(const char *str);

/**
 * @brief Get the basename (filename) from a path
 * 
 * Handles both Unix (/) and Windows (\) path separators.
 * 
 * @param path The full path
 * @return Pointer to the basename within the path string (not a copy)
 */
const char *filter_get_basename(const char *path);

/**
 * @brief Enhanced pattern matching with wildcard and case-insensitive support
 * 
 * Supports glob-style patterns (* and ?) with optional case-insensitive matching.
 * Uses fnmatch() internally with fallback for systems without FNM_CASEFOLD.
 * 
 * @param pattern The glob pattern to match against
 * @param string The string to test
 * @return 1 if pattern matches, 0 otherwise
 */
int filter_match_pattern(const char *pattern, const char *string);

/**
 * @brief Normalize a pattern string by trimming whitespace
 * 
 * Creates a new string with leading and trailing whitespace removed.
 * 
 * @param pattern The pattern to normalize
 * @return Newly allocated normalized string, or NULL on failure. Caller must free().
 */
char *filter_normalize_pattern(const char *pattern);

#ifdef __cplusplus
}
#endif

#endif /* FILTER_UTILS_H */
