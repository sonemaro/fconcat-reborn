// File: src/fconcat.h
#ifndef FCONCAT_H
#define FCONCAT_H

// Only define if not already defined
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Version information - only define if not already defined from command line
#ifndef FCONCAT_VERSION
#define FCONCAT_VERSION "2.0.0"
#endif

#ifndef FCONCAT_COPYRIGHT
#define FCONCAT_COPYRIGHT "Copyright (c) 2025 Soroush Khosravi Dehaghi"
#endif

// Platform-specific definitions
#if defined(_WIN32) || defined(_WIN64)
#define PATH_SEP '\\'
#include <windows.h>
#else
#define PATH_SEP '/'
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Include all subsystem headers
#include "core/types.h"
#include "core/error.h"
#include "core/memory.h"
#include "config/config.h"
#include "format/format.h"
#include "filter/filter.h"
#include "plugins/plugin.h"

#ifdef __cplusplus
}
#endif

#endif /* FCONCAT_H */