# Enhanced Makefile for fconcat 2.0 - Next-generation file concatenator
# Supports complex multi-module architecture with engines, plugins, and comprehensive tooling

# ============================================================================
# COMPILER AND BASIC SETTINGS
# ============================================================================

CC ?= gcc
CXX ?= g++
AR ?= ar
STRIP ?= strip

# Enhanced compiler flags for security and modern C11 features
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
SECURITY_CFLAGS = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
# Base LDFLAGS - platform-specific flags added later
LDFLAGS = -pthread
LIBS = -lm -ldl

# Version and build info
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "2.0.0-unknown")

# Properly escape the BUILD_INFO macro
CFLAGS += -DFCONCAT_VERSION=\"$(VERSION)\"

# ============================================================================
# PROJECT STRUCTURE AND SOURCE DISCOVERY
# ============================================================================

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests
PLUGIN_DIR = plugins
BENCH_DIR = benchmarks
DOCS_DIR = docs
EXAMPLES_DIR = examples

# Auto-discover source files in subdirectories
CORE_SRCS = $(wildcard $(SRC_DIR)/core/*.c)
CONFIG_SRCS = $(wildcard $(SRC_DIR)/config/*.c)
FORMAT_SRCS = $(wildcard $(SRC_DIR)/format/*.c)
FILTER_SRCS = $(SRC_DIR)/filter/filter.c $(SRC_DIR)/filter/filter_exclude.c $(SRC_DIR)/filter/filter_binary.c $(SRC_DIR)/filter/filter_symlink.c $(SRC_DIR)/filter/filter_include.c
PLUGIN_SRCS = $(SRC_DIR)/plugins/plugin.c
MAIN_SRCS = $(SRC_DIR)/main.c

# All source files for main binary (excluding individual plugin implementations)
ALL_SRCS = $(CORE_SRCS) $(CONFIG_SRCS) $(FORMAT_SRCS) $(FILTER_SRCS) $(PLUGIN_SRCS) $(MAIN_SRCS)
ALL_OBJS = $(ALL_SRCS:.c=.o)

# Plugin source files (to be built as shared libraries)
PLUGIN_IMPL_SRCS = $(filter-out $(SRC_DIR)/plugins/plugin.c, $(wildcard $(SRC_DIR)/plugins/*.c))

# Header files for dependency tracking
HEADERS = $(wildcard $(INCLUDE_DIR)/*.h) $(wildcard $(SRC_DIR)/*/*.h)

# Test sources
TEST_MAIN_SRC = $(TEST_DIR)/test_main.c
UNIT_TEST_SRCS = $(wildcard $(TEST_DIR)/unit/*.c)
INTEGRATION_TEST_SRCS = $(wildcard $(TEST_DIR)/integration/*.c)
TEST_OBJS = $(TEST_MAIN_SRC:.c=.o) $(UNIT_TEST_SRCS:.c=.o) $(INTEGRATION_TEST_SRCS:.c=.o)

# Plugin discovery
PLUGIN_SOURCES = $(wildcard $(PLUGIN_DIR)/*.c)
EXAMPLE_PLUGIN_SOURCES = $(wildcard $(EXAMPLES_DIR)/plugins/*.c)

# ============================================================================
# PLATFORM AND CROSS-COMPILATION DETECTION
# ============================================================================

# Cross-compilation settings
MINGW_PREFIX ?= x86_64-w64-mingw32
MINGW32_PREFIX ?= i686-w64-mingw32

# Platform-specific library initialization
ifeq ($(OS),Windows_NT)
    LIBS = -lm -lwinpthread -lws2_32
    EXE_SUFFIX = .exe
    PLUGIN_SUFFIX = .dll
    RM = del /Q
    MKDIR = mkdir
    PATH_SEP = \\
    IS_MACOS = 0
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        LIBS += -lrt
        IS_MACOS = 0
    else ifeq ($(UNAME_S),Darwin)
        IS_MACOS = 1
    else
        IS_MACOS = 0
    endif
    EXE_SUFFIX = 
    PLUGIN_SUFFIX = .so
    RM = rm -f
    MKDIR = mkdir -p
    PATH_SEP = /
endif

# Cross-compilation detection
ifeq ($(CROSS_COMPILE),aarch64-linux-gnu-)
    CC = aarch64-linux-gnu-gcc
    CFLAGS += -march=armv8-a
    PLUGIN_SUFFIX = .so
    IS_CROSS_COMPILE = 1
else ifeq ($(CROSS_COMPILE),mingw64)
    CC = $(MINGW_PREFIX)-gcc
    TARGET_SUFFIX = .exe
    PLUGIN_SUFFIX = .dll
    LDFLAGS = -static-libgcc -static-libstdc++
    LIBS = -lm -lwinpthread -lws2_32
    CFLAGS := $(filter-out -march=native,$(CFLAGS))
    CFLAGS += -march=x86-64
    IS_CROSS_COMPILE = 1
    IS_WINDOWS_CROSS = 1
else ifeq ($(CROSS_COMPILE),mingw32)
    CC = $(MINGW32_PREFIX)-gcc
    TARGET_SUFFIX = .exe
    PLUGIN_SUFFIX = .dll
    LDFLAGS = -static-libgcc -static-libstdc++
    LIBS = -lm -lwinpthread -lws2_32
    CFLAGS := $(filter-out -march=native,$(CFLAGS))
    CFLAGS += -march=i686
    IS_CROSS_COMPILE = 1
    IS_WINDOWS_CROSS = 1
else
    ifndef IS_CROSS_COMPILE
        CFLAGS += -march=native
    endif
    PLUGIN_SUFFIX = .so
endif

# Enhanced Windows cross-compilation detection
ifneq (,$(findstring mingw,$(CC)))
    TARGET_SUFFIX = .exe
    PLUGIN_SUFFIX = .dll
    LIBS := $(filter-out -lrt,$(LIBS))
    LIBS += -lwinpthread -lws2_32
    LDFLAGS = -static-libgcc -static-libstdc++
    LIBS := $(filter-out -ldl,$(LIBS))
    IS_WINDOWS_CROSS = 1
endif

# ============================================================================
# BUILD CONFIGURATION
# ============================================================================

# Plugin settings
PLUGIN_CFLAGS = -Wall -Wextra -O2 -fPIC -I$(INCLUDE_DIR)
PLUGIN_LDFLAGS = -shared
PLUGIN_LIBS = 

# Enhanced plugin settings for Windows
ifdef IS_WINDOWS_CROSS
    PLUGIN_CFLAGS += -DWITH_PLUGINS
    PLUGIN_LDFLAGS = -shared -Wl,--out-implib,$@.a
    PLUGIN_LIBS = -lkernel32
else
    PLUGIN_LDFLAGS = -shared -fPIC
    PLUGIN_LIBS = 
endif

# Test settings
TEST_CFLAGS = $(CFLAGS) -I$(INCLUDE_DIR) -I$(SRC_DIR) -I$(SRC_DIR)/core -DUNIT_TESTING
TEST_LDFLAGS = $(LDFLAGS)
TEST_LIBS = $(LIBS)

# Benchmark settings
BENCH_CFLAGS = -O3 -DNDEBUG -pthread -I$(INCLUDE_DIR)
BENCH_LDFLAGS = $(LDFLAGS)
BENCH_LIBS = $(LIBS)
BENCH_ITERATIONS ?= 1000
BENCH_FILE_SIZE ?= 10M

# Build targets
TARGET = fconcat$(TARGET_SUFFIX)
TEST_TARGET = test_fconcat$(TARGET_SUFFIX)
BENCH_TARGET = bench_fconcat$(TARGET_SUFFIX)

# Plugin targets - FIXED: Build from implementation sources
PLUGIN_IMPL_TARGETS = $(PLUGIN_IMPL_SRCS:$(SRC_DIR)/plugins/%.c=%$(PLUGIN_SUFFIX))
PLUGIN_TARGETS = $(PLUGIN_SOURCES:.c=$(PLUGIN_SUFFIX))
EXAMPLE_PLUGIN_TARGETS = $(EXAMPLE_PLUGIN_SOURCES:.c=$(PLUGIN_SUFFIX))

# ============================================================================
# ENHANCED BUILD MODES
# ============================================================================

# FIXED: Always enable plugins and dynamic linking for proper plugin support
CFLAGS += -DWITH_PLUGINS

# Security hardening (can be disabled with SECURITY=0)
ifndef SECURITY
    SECURITY = 1
endif

ifeq ($(SECURITY),1)
    CFLAGS += $(SECURITY_CFLAGS)
    # Linux-specific linker hardening flags (not supported on macOS)
    ifeq ($(IS_MACOS),0)
        LDFLAGS += -pie -Wl,-z,relro,-z,now
    endif
endif

# Link-time optimization
ifdef LTO
    CFLAGS += -flto
    LDFLAGS += -flto
endif

# Parallel build support
MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 4)

# ============================================================================
# PHONY TARGETS
# ============================================================================

.PHONY: all clean clean-all install uninstall test test-unit test-integration \
        plugins plugins-enabled plugins-clean plugins-examples \
        windows windows32 windows64 windows-all \
        windows-plugins windows-plugins32 windows-plugins64 windows-plugins-all \
        benchmark bench-clean bench-report profile release debug debug-plugins \
        debug-plugins-only sanitize msan tsan analyze format-check \
        docs docs-clean package dist help debug-info \
        coverage-build coverage coverage-clean

# ============================================================================
# MAIN TARGETS
# ============================================================================

all: $(TARGET)

# Build with comprehensive dependency tracking
$(TARGET): $(ALL_OBJS)
	@echo "🔗 Linking $@..."
	$(CC) $(ALL_OBJS) $(LDFLAGS) $(LIBS) -o $@
	@echo "✅ Built $@ successfully"

# Enhanced object file compilation with dependency generation
%.o: %.c $(HEADERS)
	@echo "🔨 Compiling $<..."
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -MMD -MP -c $< -o $@

# Include dependency files
-include $(ALL_OBJS:.o=.d)

# ============================================================================
# PLUGIN TARGETS
# ============================================================================

plugins-enabled: plugins

# FIXED: Build implementation plugins separately
plugins: $(PLUGIN_IMPL_TARGETS)
	@echo "✅ Built $(words $(PLUGIN_IMPL_TARGETS)) built-in plugin(s)"

plugins-examples: $(EXAMPLE_PLUGIN_TARGETS)
	@echo "✅ Built $(words $(EXAMPLE_PLUGIN_TARGETS)) example plugin(s)"

plugins-external: $(PLUGIN_TARGETS)
	@echo "✅ Built $(words $(PLUGIN_TARGETS)) external plugin(s)"

# FIXED: Build individual plugin implementations as shared libraries
%$(PLUGIN_SUFFIX): $(SRC_DIR)/plugins/%.c
	@echo "🔌 Building plugin $@..."
ifdef IS_WINDOWS_CROSS
	$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) $< -o $@ $(PLUGIN_LIBS)
else
	$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) $< -o $@ $(PLUGIN_LIBS)
endif

$(PLUGIN_DIR)/%$(PLUGIN_SUFFIX): $(PLUGIN_DIR)/%.c
	@echo "🔌 Building external plugin $@..."
ifdef IS_WINDOWS_CROSS
	$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) $< -o $@ $(PLUGIN_LIBS)
else
	$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) $< -o $@ $(PLUGIN_LIBS)
endif

$(EXAMPLES_DIR)/plugins/%$(PLUGIN_SUFFIX): $(EXAMPLES_DIR)/plugins/%.c
	@echo "🔌 Building example plugin $@..."
ifdef IS_WINDOWS_CROSS
	$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) $< -o $@ $(PLUGIN_LIBS)
else
	$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) $< -o $@ $(PLUGIN_LIBS)
endif

# ============================================================================
# TEST TARGETS
# ============================================================================

test: test-unit test-integration

test-unit: $(TEST_TARGET)
	@echo "🧪 Running unit tests..."
	./$(TEST_TARGET) --unit

test-integration: $(TEST_TARGET)
	@echo "🧪 Running integration tests..."
	./$(TEST_TARGET) --integration

$(TEST_TARGET): $(filter-out $(SRC_DIR)/main.o,$(ALL_OBJS)) $(TEST_OBJS)
	@echo "🔗 Linking test executable..."
	$(CC) $^ $(TEST_LDFLAGS) $(TEST_LIBS) -o $@

$(TEST_DIR)/%.o: $(TEST_DIR)/%.c $(HEADERS)
	@echo "🔨 Compiling test $<..."
	$(CC) $(TEST_CFLAGS) -MMD -MP -c $< -o $@

# ============================================================================
# BENCHMARK TARGETS
# ============================================================================

benchmark: $(BENCH_TARGET)
	@echo "📊 Running benchmarks..."
	./$(BENCH_TARGET) $(BENCH_ITERATIONS) $(BENCH_FILE_SIZE)

$(BENCH_TARGET): $(wildcard $(BENCH_DIR)/*.c) $(filter-out $(SRC_DIR)/main.o,$(ALL_OBJS))
	@echo "🔗 Linking benchmark executable..."
	$(CC) $(BENCH_CFLAGS) -o $@ $^ $(BENCH_LDFLAGS) $(BENCH_LIBS)

bench-report: benchmark
	@echo "📊 Benchmark Report:"
	@echo "===================="
	@echo "Version: $(VERSION)"
	@echo "Compiler: $(CC)"
	@echo "Optimization: $(BENCH_CFLAGS)"
	@echo "Iterations: $(BENCH_ITERATIONS)"
	@echo "File size: $(BENCH_FILE_SIZE)"
	@echo

# ============================================================================
# ENHANCED BUILD VARIANTS
# ============================================================================

# macOS minimum version for release builds (ensures 5-year compatibility)
MACOS_MIN_VERSION ?= 12.0

# Release build with maximum optimization
# IMPORTANT: No -march=native for portability across different CPUs
release:
	@echo "🚀 Building optimized release..."
	$(MAKE) clean
ifeq ($(UNAME_S),Darwin)
	$(MAKE) CFLAGS="-Wall -Wextra -Werror -std=c11 -O3 -DNDEBUG -flto -ffast-math -DWITH_PLUGINS -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -mmacosx-version-min=$(MACOS_MIN_VERSION)" LDFLAGS="-pthread -flto" LIBS="-lm -ldl" $(TARGET)
else
	$(MAKE) CFLAGS="-Wall -Wextra -Werror -std=c11 -O3 -DNDEBUG -flto -ffast-math -DWITH_PLUGINS -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L" LDFLAGS="-pthread -flto -pie -Wl,-z,relro,-z,now" LIBS="-lm -ldl -lrt" $(TARGET)
endif
	@echo "✅ Release build complete"

# Debug build with comprehensive debugging
debug:
	@echo "🐛 Building debug version..."
	$(MAKE) clean
ifeq ($(UNAME_S),Darwin)
	$(MAKE) CFLAGS="-Wall -Wextra -std=c11 -g3 -O0 -DDEBUG -DFCONCAT_DEBUG -DWITH_PLUGINS -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L" LDFLAGS="-pthread -g" LIBS="-lm -ldl" $(TARGET)
else
	$(MAKE) CFLAGS="-Wall -Wextra -std=c11 -g3 -O0 -DDEBUG -DFCONCAT_DEBUG -DWITH_PLUGINS -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L" LDFLAGS="-pthread -g" LIBS="-lm -ldl -lrt" $(TARGET)
endif
	@echo "✅ Debug build complete"

# Debug with plugin support
debug-plugins: debug plugins

# Debug plugins only
debug-plugins-only: PLUGIN_CFLAGS = -Wall -Wextra -O0 -g3 -fPIC -I$(INCLUDE_DIR)
debug-plugins-only: plugins plugins-examples

# Profiling build
profile: CFLAGS += -pg -g
profile: LDFLAGS += -pg
profile: $(TARGET)
	@echo "📈 Built with profiling support"

# ============================================================================
# SANITIZERS AND ANALYSIS
# ============================================================================

# AddressSanitizer build
sanitize: CFLAGS += -fsanitize=address -fsanitize=undefined -g -O1
sanitize: LDFLAGS += -fsanitize=address -fsanitize=undefined
sanitize: $(TARGET)
	@echo "🔬 Built with AddressSanitizer and UBSan"

# MemorySanitizer build (requires clang)
msan: CC = clang
msan: CFLAGS += -fsanitize=memory -fno-omit-frame-pointer -g -O1
msan: LDFLAGS += -fsanitize=memory
msan: $(TARGET)
	@echo "🔬 Built with MemorySanitizer"

# ThreadSanitizer build
tsan: CFLAGS += -fsanitize=thread -g -O1
tsan: LDFLAGS += -fsanitize=thread
tsan: $(TARGET)
	@echo "🔬 Built with ThreadSanitizer"

# Static analysis (requires cppcheck)
analyze:
	@echo "🔍 Running static analysis..."
	@command -v cppcheck >/dev/null 2>&1 || { echo "cppcheck not found. Install it for static analysis."; exit 1; }
	cppcheck --enable=all --std=c11 --suppress=missingIncludeSystem -I$(INCLUDE_DIR) $(SRC_DIR)

# Format checking (requires clang-format)
format-check:
	@echo "🎨 Checking code formatting..."
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found."; exit 1; }
	@find $(SRC_DIR) $(INCLUDE_DIR) -name "*.c" -o -name "*.h" | xargs clang-format -style=file -dry-run -Werror

# ============================================================================
# CODE COVERAGE
# ============================================================================

# Coverage instrumentation flags
COVERAGE_CFLAGS = -fprofile-arcs -ftest-coverage -O0 -g
COVERAGE_LDFLAGS = -lgcov --coverage

# Build with coverage instrumentation
coverage-build: CFLAGS += $(COVERAGE_CFLAGS)
coverage-build: LDFLAGS += $(COVERAGE_LDFLAGS)
coverage-build: $(TARGET) $(TEST_TARGET)
	@echo "📊 Built with coverage instrumentation"

# Run tests and generate coverage report
coverage: clean coverage-build
	@echo "📊 Running tests with coverage..."
	./$(TEST_TARGET)
	@echo "📊 Generating coverage report..."
	@command -v lcov >/dev/null 2>&1 || { echo "lcov not found. Install lcov for coverage reports."; exit 1; }
	lcov --capture --directory . --output-file coverage.info --rc lcov_branch_coverage=1
	lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage.info --rc lcov_branch_coverage=1
	@command -v genhtml >/dev/null 2>&1 || { echo "genhtml not found. Install lcov for HTML reports."; exit 1; }
	genhtml coverage.info --output-directory coverage_html --branch-coverage
	@echo "📊 Coverage report generated: coverage_html/index.html"
	@lcov --summary coverage.info

# Clean coverage artifacts
coverage-clean:
	@echo "🧹 Cleaning coverage artifacts..."
	find . -name "*.gcda" -delete
	find . -name "*.gcno" -delete
	rm -f coverage.info
	rm -rf coverage_html

# ============================================================================
# CROSS-COMPILATION TARGETS (Enhanced)
# ============================================================================

windows: windows64

windows64:
	@echo "🪟 Building Windows 64-bit version..."
	$(MAKE) CROSS_COMPILE=mingw64 TARGET=fconcat-win64.exe clean all

windows32:
	@echo "🪟 Building Windows 32-bit version..."
	$(MAKE) CROSS_COMPILE=mingw32 TARGET=fconcat-win32.exe clean all

windows-all: windows64 windows32

windows-plugins: windows-plugins64

windows-plugins64:
	@echo "🪟 Building Windows 64-bit plugins..."
	$(MAKE) CROSS_COMPILE=mingw64 plugins plugins-examples

windows-plugins32:
	@echo "🪟 Building Windows 32-bit plugins..."
	$(MAKE) CROSS_COMPILE=mingw32 plugins plugins-examples

windows-plugins-all: windows-plugins64 windows-plugins32

# ============================================================================
# DOCUMENTATION
# ============================================================================

docs:
	@echo "📚 Generating documentation..."
	@command -v doxygen >/dev/null 2>&1 || { echo "doxygen not found."; exit 1; }
	doxygen Doxyfile

docs-clean:
	$(RM) -r docs/html docs/latex

# ============================================================================
# INSTALLATION AND PACKAGING
# ============================================================================

install: $(TARGET)
	@echo "📦 Installing fconcat..."
	$(MKDIR) $(DESTDIR)/usr/local/bin
	$(MKDIR) $(DESTDIR)/usr/local/include/fconcat
	$(MKDIR) $(DESTDIR)/usr/local/lib/fconcat
	cp $(TARGET) $(DESTDIR)/usr/local/bin/
	cp $(INCLUDE_DIR)/*.h $(DESTDIR)/usr/local/include/fconcat/
	@echo "✅ Installation complete"

uninstall:
	@echo "🗑️  Uninstalling fconcat..."
	$(RM) $(DESTDIR)/usr/local/bin/fconcat
	$(RM) -r $(DESTDIR)/usr/local/include/fconcat
	$(RM) -r $(DESTDIR)/usr/local/lib/fconcat

# Create distribution package
dist: clean
	@echo "📦 Creating distribution package..."
	tar -czf fconcat-$(VERSION).tar.gz \
		--exclude-vcs \
		--exclude='*.tar.gz' \
		--exclude='*.o' \
		--exclude='*.d' \
		--exclude='.vscode' \
		.

package: release
	@echo "📦 Creating release package..."
	$(MKDIR) fconcat-$(VERSION)
	cp $(TARGET) fconcat-$(VERSION)/
	cp README.md LICENSE fconcat-$(VERSION)/ 2>/dev/null || true
	cp -r $(DOCS_DIR) fconcat-$(VERSION)/ 2>/dev/null || true
	tar -czf fconcat-$(VERSION)-$(shell uname -m).tar.gz fconcat-$(VERSION)
	$(RM) -r fconcat-$(VERSION)

# ============================================================================
# CLEANUP TARGETS
# ============================================================================

clean:
	@echo "🧹 Cleaning build artifacts..."
	$(RM) $(ALL_OBJS) $(TEST_OBJS) $(TARGET) $(TEST_TARGET) $(BENCH_TARGET)
	$(RM) $(ALL_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
	$(RM) gmon.out

plugins-clean:
	@echo "🧹 Cleaning plugin artifacts..."
	$(RM) *$(PLUGIN_SUFFIX)
	$(RM) $(PLUGIN_DIR)/*$(PLUGIN_SUFFIX) 2>/dev/null || true
	$(RM) $(EXAMPLES_DIR)/plugins/*$(PLUGIN_SUFFIX) 2>/dev/null || true
	$(RM) $(PLUGIN_DIR)/*.a 2>/dev/null || true
	$(RM) $(EXAMPLES_DIR)/plugins/*.a 2>/dev/null || true

bench-clean:
	@echo "🧹 Cleaning benchmark artifacts..."
	$(RM) $(BENCH_TARGET)
	$(RM) $(BENCH_DIR)/*.tmp 2>/dev/null || true

clean-all: clean plugins-clean bench-clean docs-clean
	@echo "🧹 Cleaning all artifacts..."
	$(RM) fconcat-win64.exe fconcat-win32.exe
	$(RM) fconcat-*.tar.gz
	$(RM) -r fconcat-*/ 2>/dev/null || true

# ============================================================================
# DEBUG AND HELP
# ============================================================================

debug-info:
	@echo "🔍 Build Configuration:"
	@echo "======================"
	@echo "CC: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "LIBS: $(LIBS)"
	@echo "VERSION: $(VERSION)"
	@echo "PLUGIN_SUFFIX: $(PLUGIN_SUFFIX)"
	@echo "SECURITY: $(SECURITY)"
	@echo "IS_WINDOWS_CROSS: $(IS_WINDOWS_CROSS)"
	@echo "IS_CROSS_COMPILE: $(IS_CROSS_COMPILE)"
	@echo "SOURCE_FILES: $(words $(ALL_SRCS))"
	@echo "HEADER_FILES: $(words $(HEADERS))"
	@echo "PLUGIN_SOURCES: $(words $(PLUGIN_SOURCES))"
	@echo "PLUGIN_IMPL_TARGETS: $(PLUGIN_IMPL_TARGETS)"
	@echo "TEST_SOURCES: $(words $(UNIT_TEST_SRCS)) unit + $(words $(INTEGRATION_TEST_SRCS)) integration"

help:
	@echo "🚀 fconcat 2.0 Build System"
	@echo "============================"
	@echo
	@echo "Basic targets:"
	@echo "  all                 - Build fconcat (default)"
	@echo "  clean               - Clean build artifacts"
	@echo "  clean-all           - Clean everything"
	@echo "  install             - Install to system"
	@echo "  uninstall           - Remove from system"
	@echo "  help                - Show this help"
	@echo
	@echo "Build variants:"
	@echo "  release             - Optimized release build"
	@echo "  debug               - Debug build with symbols"
	@echo "  debug-plugins       - Debug build with plugins"
	@echo "  profile             - Build with profiling"
	@echo "  sanitize            - Build with sanitizers"
	@echo
	@echo "Plugin support:"
	@echo "  plugins             - Build built-in plugins as shared libraries"
	@echo "  plugins-examples    - Build example plugins"
	@echo "  plugins-external    - Build external plugins"
	@echo "  plugins-clean       - Clean plugin artifacts"
	@echo "  debug-plugins-only  - Build only plugins with debug"
	@echo
	@echo "Testing:"
	@echo "  test                - Run all tests"
	@echo "  test-unit           - Run unit tests"
	@echo "  test-integration    - Run integration tests"
	@echo
	@echo "Benchmarking:"
	@echo "  benchmark           - Run performance benchmarks"
	@echo "  bench-report        - Generate benchmark report"
	@echo "  bench-clean         - Clean benchmark artifacts"
	@echo
	@echo "Cross-compilation:"
	@echo "  windows             - Build for Windows 64-bit"
	@echo "  windows32           - Build for Windows 32-bit"
	@echo "  windows64           - Build for Windows 64-bit"
	@echo "  windows-all         - Build for all Windows targets"
	@echo "  windows-plugins*    - Build plugins for Windows"
	@echo
	@echo "Analysis and docs:"
	@echo "  analyze             - Run static analysis"
	@echo "  format-check        - Check code formatting"
	@echo "  docs                - Generate documentation"
	@echo "  docs-clean          - Clean documentation"
	@echo
	@echo "Packaging:"
	@echo "  dist                - Create source distribution"
	@echo "  package             - Create binary package"
	@echo
	@echo "Debug info:"
	@echo "  debug-info          - Show build configuration"
	@echo
	@echo "Environment variables:"
	@echo "  SECURITY=0          - Disable security hardening"
	@echo "  LTO=1               - Enable link-time optimization"
	@echo "  MINGW_PREFIX        - MinGW 64-bit prefix"
	@echo "  MINGW32_PREFIX      - MinGW 32-bit prefix"
	@echo "  BENCH_ITERATIONS    - Benchmark iteration count"
	@echo "  BENCH_FILE_SIZE     - Benchmark file size"
	@echo
	@echo "Usage examples:"
	@echo "  make debug-plugins                          # Build with plugins"
	@echo "  make release LTO=1"
	@echo "  make test"
	@echo "  make windows-all"
	@echo "  make benchmark BENCH_ITERATIONS=5000"
	@echo "  ./fconcat ./src output.txt --plugin ./end_marker_filter.so"