#ifndef CORE_ERROR_H
#define CORE_ERROR_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Error context
typedef struct {
    FconcatErrorCode code;
    char *message;
    char *file;
    int line;
    char *function;
    time_t timestamp;
} ErrorContext;

// Error manager
typedef struct {
    ErrorContext errors[1000];
    int error_count;
    int warning_count;
    pthread_mutex_t mutex;
} ErrorManager;

// Error handling functions
ErrorManager *error_manager_create(void);
void error_manager_destroy(ErrorManager *manager);
void error_report(ErrorManager *manager, FconcatErrorCode code, const char *format, ...);
void error_report_context(ErrorManager *manager, FconcatErrorCode code, const char *file, int line, const char *function, const char *format, ...);
void warning_report(ErrorManager *manager, const char *format, ...);
int error_get_count(ErrorManager *manager);
int warning_get_count(ErrorManager *manager);
void error_clear(ErrorManager *manager);

// Error macros
#define ERROR_REPORT(mgr, code, ...) error_report_context(mgr, code, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define WARNING_REPORT(mgr, ...) warning_report(mgr, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* CORE_ERROR_H */
