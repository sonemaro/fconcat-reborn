fconcat - File Concatenator
===========================

`fconcat` is a small C11 utility for turning a directory tree into one
streamed text document. It is aimed at code review, LLM context preparation,
documentation snapshots, and archival workflows where predictable output and
bounded memory behavior matter.

The project has two modes:

- Batch mode writes one output file.
- Server mode exposes an HTTP endpoint that streams concatenation results from
  explicitly allowed roots.

Installation
------------

### Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/sonemaro/fconcat-reborn/main/.github/scripts/install.sh | sh
```

### Build From Source

Requirements:

- C11 compiler: GCC, Clang, or compatible `cc`
- POSIX threads
- GNU Make

```bash
git clone https://github.com/sonemaro/fconcat-reborn.git
cd fconcat-reborn
make
sudo make install
```

Usage
-----

### Batch Mode

```bash
fconcat <input_directory> <output_file> [options]
```

Examples:

```bash
fconcat ./src output.txt
fconcat ./project result.txt --include "*.c" "*.h"
fconcat ./kernel out.txt --exclude "*.o" "build/*" "test*"
fconcat ./src out.txt --include "*.py" --exclude "__pycache__/*"
fconcat ./code result.txt --show-size --binary-placeholder
```

Batch options:

```text
--include <patterns>        Include only files matching patterns
--exclude <patterns>        Exclude files matching patterns
--show-size, -s             Show file sizes in the tree
--verbose, -v               Enable debug logging
--log-level <level>         error, warning, info, debug, trace
--binary-skip               Skip binary files entirely (default)
--binary-include            Include binary file bytes
--binary-placeholder        Emit a placeholder for binary file content
--symlinks <mode>           skip, follow, include, placeholder
```

### Server Mode

```bash
fconcat --serve --listen 127.0.0.1:8080 --allow-root /path/to/root
```

Server options:

```text
--listen <host:port>        Address to bind
--allow-root <path>         Allowed root; repeat for multiple roots
--workers <n>               Worker threads, default 4
--queue <n>                 Pending connection queue, default 64
--auth-token <token>        Optional bearer token for all requests
--verbose, -v               Enable debug logging
--log-level <level>         error, warning, info, debug, trace
```

Endpoints:

```text
GET /healthz
GET /concat?root=<path>&include=<glob>&exclude=<glob>&show_size=1
    &binary=skip|include|placeholder&symlinks=skip|follow|include|placeholder
```

When `--auth-token` is set, requests must include:

```text
Authorization: Bearer <token>
```

Pattern Matching
----------------

Patterns support `*` for any sequence of characters and `?` for any single
character. Patterns match against both full paths and basenames.

Output
------

Output is deterministic plain text. Directory entries are emitted in
lexicographic order within each directory. There is no JSON mode and no plugin
system.

```text
Directory Structure:
==================

DIR  src/
  FILE main.c

File Contents:
=============

// File: src/main.c
#include <stdio.h>
...
```

Safety Model
------------

- Directory traversal is iterative, not recursive, so deep trees do not consume
  process stack.
- File contents stream in fixed-size chunks instead of being buffered as whole
  files.
- Binary and symlink behavior is explicit and test-covered.
- Visited directories are tracked by device/inode to avoid filesystem cycles.
- Allocations are centralized through the existing memory manager where context
  ownership is needed, and every request in server mode builds and tears down
  its own processing state.
- Zero-leak release contract: `make sanitize-test` is a blocking gate. It runs
  the full test suite with AddressSanitizer, UndefinedBehaviorSanitizer, and a
  project-local allocation leak guard, including simulated allocation-failure
  subprocesses; any live allocation at process exit is a release failure.

Development
-----------

```bash
make                # Build fconcat
make test           # Build and run the test suite
make sanitize-test  # Run tests under ASan/UBSan plus leak guard
make release        # Optimized build
make bench          # Raw traversal benchmark to /dev/null
make bench-real     # Full output benchmark with bytes written
make clean          # Remove local build outputs
```

Useful benchmark overrides:

```bash
make bench BENCH_ROOT=/path/to/tree BENCH_ITERATIONS=10
make bench-real BENCH_ROOT=/path/to/tree BENCH_OUTPUT=/tmp/fconcat.txt
make bench-real BENCH_ROOT=/path/to/tree BENCH_WARMUP=0
BENCH_ROOT=/path/to/tree sh scripts/bench.sh check
```

Benchmarks print the binary path, root, file count, disk usage, platform, run
times, warmup setting, and a min/avg/max summary so results can be compared
across runs.

The codebase is organized by subsystem:

```text
src/
├── main.c
├── config/         # Strict CLI/default configuration
├── core/           # Context, traversal, memory, errors
├── filter/         # Include/exclude, binary, symlink rules
├── output/         # Streaming text output sinks
└── server/         # HTTP streaming server and worker pool
```

Contributing
------------

- Keep the CLI strict: unknown or removed flags must fail.
- Keep output streaming; avoid whole-file buffering in normal paths.
- Add or update tests when behavior changes.
- Run `make test` before submitting changes.
- Run `make sanitize-test` before release work.

License
-------

Copyright (c) 2025 Soroush Khosravi Dehaghi

See source files for license details.
