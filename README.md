fconcat - File Concatenator
============================

A fast, memory-safe file concatenation utility for developers who need to
combine directory contents into a single document. Written in C11 for
maximum portability and performance.

fconcat recursively traverses directories and concatenates file contents
with structural headers, making it ideal for code review, documentation,
LLM context preparation, or archival purposes.

Installation
------------

### Quick Install (Recommended)

**Linux & macOS:**

```bash
curl -fsSL https://raw.githubusercontent.com/sonemaro/fconcat-reborn/main/.github/scripts/install.sh | sh
```

**Install a specific version:**

```bash
curl -fsSL https://raw.githubusercontent.com/sonemaro/fconcat-reborn/main/.github/scripts/install.sh | FCONCAT_VERSION=v1.0.0 sh
```

**Install to custom directory:**

```bash
curl -fsSL https://raw.githubusercontent.com/sonemaro/fconcat-reborn/main/.github/scripts/install.sh | FCONCAT_INSTALL_DIR=~/.local/bin sh
```

### Manual Download

Download the binary for your platform from [Releases](https://github.com/sonemaro/fconcat-reborn/releases):

| Platform | Architecture | Binary |
|----------|--------------|--------|
| Linux | x86_64 | `fconcat-linux-amd64` |
| Linux | ARM64 | `fconcat-linux-arm64` |
| macOS | Intel | `fconcat-macos-amd64` |
| macOS | Apple Silicon | `fconcat-macos-arm64` |

Then:

```bash
chmod +x fconcat-*
sudo mv fconcat-* /usr/local/bin/fconcat
```

### Build from Source

Requirements:
- GCC or Clang with C11 support
- GNU Make

```bash
git clone https://github.com/sonemaro/fconcat-reborn.git
cd fconcat-reborn
make release
sudo make install
```

Usage
-----

```
fconcat <input_directory> <output_file> [options]
```

### Basic Examples

```bash
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
```

Options
-------

```
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
```

Pattern Matching
----------------

Patterns support wildcards:
- `*` matches any sequence of characters
- `?` matches any single character

Patterns match against both full paths and basenames. Multiple patterns
can be specified:

```bash
fconcat ./src out.txt --include "*.c" "*.h" "Makefile"
fconcat ./proj out.txt --exclude "*.log" "*.tmp" "cache/*"
```

Output Formats
--------------

**Text format** (default):

```
================================================================================
File: src/main.c
================================================================================
#include <stdio.h>
...
```

**JSON format**:

```json
{
  "files": [
    {
      "path": "src/main.c",
      "size": 4096,
      "content": "..."
    }
  ]
}
```

Plugin System
-------------

fconcat supports loadable plugins for custom processing. Plugins are
shared libraries that implement the plugin API:

```c
PLUGIN_EXPORT int plugin_init(FconcatContext *ctx);
PLUGIN_EXPORT void plugin_destroy(FconcatContext *ctx);
PLUGIN_EXPORT int plugin_process_file(FconcatContext *ctx,
                                      const char *path,
                                      const char *content,
                                      size_t size);
```

Load a plugin:

```bash
fconcat ./src out.txt --plugin ./myplugin.so
fconcat ./src out.txt --plugin ./myplugin.so:debug=1,threshold=100
```

**Note:** All release binaries are dynamically linked to support plugin loading.

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

```
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
```

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

Development
-----------

### Building

```bash
make              # Standard build
make debug        # Debug build with symbols
make release      # Optimized release build
make test         # Run all tests
make sanitize     # Build with AddressSanitizer
```

### Testing

```bash
make test              # Run all tests
make test-unit         # Run only unit tests
make test-integration  # Run only integration tests
make coverage          # Generate coverage report (requires lcov)
```

The test suite includes 81 tests covering memory management, filters,
configuration parsing, and directory traversal.

### Sanitizers

```bash
make sanitize    # AddressSanitizer + UBSan
make msan        # MemorySanitizer (requires clang)
make tsan        # ThreadSanitizer
```

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
- Releases: https://github.com/sonemaro/fconcat-reborn/releases
