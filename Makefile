TARGET := fconcat
TEST_TARGET := test_fconcat

CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CPPFLAGS ?= -Iinclude -Isrc -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
WARNFLAGS ?= -Wall -Wextra -Werror -std=c11
LDFLAGS ?=
LDLIBS ?= -lm -pthread
EXTRA_CPPFLAGS ?=
EXTRA_SOURCES ?=

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "2.0.0-unknown")
CPPFLAGS += -DFCONCAT_VERSION=\"$(VERSION)\" $(EXTRA_CPPFLAGS)

SOURCES := \
	src/config/config.c \
	src/core/context.c \
	src/core/error.c \
	src/core/file_index.c \
	src/core/memory.c \
	src/filter/filter.c \
	src/filter/filter_binary.c \
	src/filter/filter_exclude.c \
	src/filter/filter_include.c \
	src/filter/filter_symlink.c \
	src/filter/filter_utils.c \
	src/output/output.c \
	src/server/server.c \
	src/main.c

TEST_SOURCES := \
	tests/test_main.c \
	tests/unit/test_config.c \
	tests/unit/test_filter.c \
	tests/unit/test_memory.c \
	tests/unit/test_output.c \
	tests/integration/test_traversal.c

OBJECTS := $(SOURCES:.c=.o)
TEST_OBJECTS := $(TEST_SOURCES:.c=.o)
EXTRA_OBJECTS := $(EXTRA_SOURCES:.c=.o)
DEPS := $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(EXTRA_OBJECTS:.o=.d)

SANITIZER_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
SANITIZER_LDFLAGS := -fsanitize=address,undefined \
	-Wl,--wrap=malloc \
	-Wl,--wrap=calloc \
	-Wl,--wrap=realloc \
	-Wl,--wrap=free \
	-Wl,--wrap=strdup \
	-Wl,--wrap=realpath
SANITIZER_ENV := ASAN_OPTIONS=halt_on_error=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0:print_stacktrace=1
ANALYZE_OUTPUT ?= $(CURDIR)/build/scan-build

.PHONY: all test sanitize-test analyze release bench bench-real install uninstall clean

all: $(TARGET)

$(TARGET): $(OBJECTS) $(EXTRA_OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_TARGET): $(filter-out src/main.o,$(OBJECTS)) $(TEST_OBJECTS) $(EXTRA_OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET)

sanitize-test:
	$(MAKE) clean
	$(MAKE) CC=clang \
		EXTRA_CPPFLAGS="-DFCONCAT_LEAK_GUARD" \
		CFLAGS="$(SANITIZER_FLAGS)" \
		LDFLAGS="$(SANITIZER_LDFLAGS)" \
		EXTRA_SOURCES="tests/leak_guard.c" \
		$(TARGET) $(TEST_TARGET)
	$(SANITIZER_ENV) ./$(TEST_TARGET)

analyze:
	command -v scan-build >/dev/null
	$(MAKE) clean
	scan-build --status-bugs -o "$(ANALYZE_OUTPUT)" $(MAKE) -B $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET)

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O3 -DNDEBUG" all

bench: release
	@BENCH_BIN=./$(TARGET) sh scripts/bench.sh raw

bench-real: release
	@BENCH_BIN=./$(TARGET) sh scripts/bench.sh real

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"

clean:
	rm -f "$(TARGET)" "$(TEST_TARGET)"
	find src tests -type f \( -name '*.o' -o -name '*.d' \) -delete

-include $(DEPS)
