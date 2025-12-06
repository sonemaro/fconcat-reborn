fconcat - File Concatenator
============================

A fast, memory-safe file concatenation utility for developers who need to
combine directory contents into a single document. Written in C11 for
maximum portability and performance.

fconcat recursively traverses directories and concatenates file contents
with structural headers, making it ideal for code review, documentation,
LLM context preparation, or archival purposes.

Building
--------

Requirements:
- GCC or Clang with C11 support
- GNU Make
- pthreads
- Linux/Unix or Windows (via MinGW cross-compilation)

Quick start:

    make
    ./fconcat ./src output.txt

For a debug build:

    make debug

For release with link-time optimization:

    make release LTO=1

Cross-compile for Windows:

    make windows64

Installation
------------

    make install PREFIX=/usr/local

Or just copy the binary somewhere in your PATH:

    cp fconcat /usr/local/bin/

Usage
-----

    fconcat <input_directory> <output_file> [options]

Basic examples:

    # Concatenate all files in src/ to output.txt
    fconcat ./src output.txt

    # Include only C source files
    fconcat ./project result.txt --include "*.c" "*.h"

    # Exclude build artifacts and tests
    fconcat ./kernel out.txt --exclude "*.o" "build/*" "test*"

    # JSON output with file sizes
    fconcat ./code result.json --format json --show-size

    # Combine include and exclude for fine-grained control
    fconcat ./src out.txt --include "*.py" --exclude "__pycache__/*"

Options
-------

    --include <patterns>    Include only files matching patterns
    --exclude <patterns>    Exclude files matching patterns
    --show-size, -s         Display file sizes in output
    --verbose, -v           Enable debug logging
    --log-level <level>     Set log level: error, warning, info, debug, trace
    --format <format>       Output format: text (default), json
    --binary-skip           Skip binary files (default)
    --binary-include        Include binary file contents
    --binary-placeholder    Show placeholder for binary files
    --symlinks <mode>       Symlink handling: skip, follow, include, placeholder
    --plugin <spec>         Load plugin with optional params (path:key=val,...)
    --interactive           Keep plugins active after processing

Pattern Matching
----------------

Patterns support wildcards:
- `*` matches any sequence of characters
- `?` matches any single character

Patterns match against both full paths and basenames. Multiple patterns
can be specified:

    fconcat ./src out.txt --include "*.c" "*.h" "Makefile"
    fconcat ./proj out.txt --exclude "*.log" "*.tmp" "cache/*"

Output Formats
--------------

**Text format** (default):

    ================================================================================
    File: src/main.c
    ================================================================================
    #include <stdio.h>
    ...

**JSON format**:

    {
      "files": [
        {
          "path": "src/main.c",
          "size": 4096,
          "content": "..."
        }
      ]
    }

Signal Handling
---------------

fconcat handles signals gracefully:

- `Ctrl+C` (SIGINT): First press initiates graceful shutdown, second press
  forces immediate termination
- `Ctrl+\` (SIGQUIT): Immediate shutdown
- SIGTERM: Graceful termination

Architecture
------------

The codebase is organized into subsystems:

    src/
    ├── main.c           # Entry point, CLI parsing
    ├── fconcat.h        # Main header
    ├── core/
    │   ├── context.c    # Processing context, directory traversal
    │   ├── memory.c     # Memory management with tracking
    │   ├── error.c      # Error handling
    │   └── types.h      # Core type definitions
    ├── config/
    │   └── config.c     # Configuration parsing
    ├── filter/
    │   ├── filter.c         # Filter engine
    │   ├── filter_binary.c  # Binary file detection
    │   ├── filter_exclude.c # Exclusion patterns
    │   ├── filter_include.c # Inclusion patterns
    │   └── filter_symlink.c # Symlink handling
    ├── format/
    │   ├── format.c     # Format engine
    │   └── format_text.c # Text output formatter
    └── plugins/
        └── plugin.c     # Plugin system

Safety Features
---------------

fconcat is designed with safety in mind:

**Iterative directory traversal**: Uses an explicit stack instead of
recursion, preventing stack overflow on deeply nested directories
(tested up to 256 levels).

**Memory protection**: Three-layer memory safety with header magic,
freed pointer detection, and tail canaries for buffer overflow detection.
All magic values are randomized at startup from /dev/urandom.

**Filesystem cycle detection**: Tracks visited directories by device/inode
pair to prevent infinite loops from symlink cycles or bind mounts.

**Compile-time hardening**: Built with stack protector, FORTIFY_SOURCE,
position-independent code, and full RELRO.

Testing
-------

Run the test suite:

    make test

Run only unit tests:

    make test-unit

Run only integration tests:

    make test-integration

Generate coverage report (requires lcov):

    make coverage

The test suite includes 81 tests covering memory management, filters,
configuration parsing, and directory traversal.

Sanitizers
----------

Build with AddressSanitizer:

    make sanitize

Build with MemorySanitizer (requires clang):

    make msan

Build with ThreadSanitizer:

    make tsan

Plugin System
-------------

fconcat supports loadable plugins for custom processing. Plugins are
shared libraries that implement the plugin API:

    PLUGIN_EXPORT int plugin_init(FconcatContext *ctx);
    PLUGIN_EXPORT void plugin_destroy(FconcatContext *ctx);
    PLUGIN_EXPORT int plugin_process_file(FconcatContext *ctx,
                                          const char *path,
                                          const char *content,
                                          size_t size);

Load a plugin:

    fconcat ./src out.txt --plugin ./myplugin.so
    fconcat ./src out.txt --plugin ./myplugin.so:debug=1,threshold=100

Docker Build
------------

Build a portable binary with older glibc for maximum compatibility:

    ./build-docker.sh

This produces a statically linked binary that runs on most Linux systems.

Benchmarking
------------

Run benchmarks:

    make benchmark

Generate benchmark report:

    make bench-report

Files
-----

- `fconcat` - Main binary
- `test_fconcat` - Test executable (not tracked in git)
- `coverage_html/` - Coverage report (generated)
- `Makefile` - Build system
- `Dockerfile` - Portable build environment

Contributing
------------

1. Fork the repo
2. Create a feature branch
3. Write tests for new functionality
4. Ensure `make test` passes
5. Submit a pull request

The code follows a straightforward C style:
- 4-space indentation
- Braces on same line
- Prefix functions with subsystem name (filter_, format_, etc.)
- Check all return values, free all allocations

Known Limitations
-----------------

- Maximum directory depth: 256 levels
- Maximum visited directories: 256 (for cycle detection)
- Binary detection uses a heuristic (null bytes in first 8KB)
- No support for character encodings other than UTF-8/ASCII

License
-------

Copyright (c) 2025 Soroush Khosravi Dehaghi

See source files for license details.

Links
-----

- Repository: https://github.com/sonemaro/fconcat-reborn
- Issues: https://github.com/sonemaro/fconcat-reborn/issues
