#ifndef CORE_FILE_INDEX_H
#define CORE_FILE_INDEX_H

#include "types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct FconcatContext;
struct FilterEngine;

#define FILE_INDEX_DEFAULT_PREFIX_BUDGET (256ULL * 1024ULL * 1024ULL)
#define FILE_INDEX_PREFIX_LIMIT (64ULL * 1024ULL)

typedef struct
{
    char *full_path;
    char *relative_path;
    int level;
    FileInfo info;
    char *prefix_data;
    size_t prefix_size;
    int prefix_complete;
} FileIndexEntry;

typedef struct FileIndex FileIndex;

FileIndex *file_index_create(size_t prefix_budget);
void file_index_destroy(FileIndex *index);
int file_index_build(FileIndex *index,
                     struct FconcatContext *ctx,
                     const ResolvedConfig *config,
                     struct FilterEngine *filters,
                     int (*should_stop)(void *user_data),
                     void *user_data);
size_t file_index_count(const FileIndex *index);
FileIndexEntry *file_index_entry(FileIndex *index, size_t offset);
size_t file_index_prefix_used(const FileIndex *index);
size_t file_index_prefix_budget(const FileIndex *index);

#ifdef __cplusplus
}
#endif

#endif /* CORE_FILE_INDEX_H */
