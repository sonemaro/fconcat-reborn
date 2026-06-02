CC ?= cc
AR ?= ar
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET := fconcat
TEST_TARGET := test_fconcat

CPPFLAGS ?= -Iinclude -Isrc -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
WARNFLAGS := -Wall -Wextra -Werror -std=c11
LDFLAGS ?=
LDLIBS ?= -lm -pthread
EXTRA_CPPFLAGS ?=
SANITIZER_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
SANITIZER_ENV := ASAN_OPTIONS=halt_on_error=1:abort_on_error=0
ALLOC_WRAP_LDFLAGS := -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=strdup -Wl,--wrap=realpath

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "2.0.0-unknown")
CPPFLAGS += -DFCONCAT_VERSION=\"$(VERSION)\"
CPPFLAGS += $(EXTRA_CPPFLAGS)

SRC := \
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

TEST_SRC := \
	tests/test_main.c \
	tests/unit/test_config.c \
	tests/unit/test_filter.c \
	tests/unit/test_memory.c \
	tests/integration/test_traversal.c

EXTRA_SRC ?=

OBJ := $(SRC:.c=.o)
TEST_OBJ := $(TEST_SRC:.c=.o)
EXTRA_OBJ := $(EXTRA_SRC:.c=.o)
DEP := $(OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(EXTRA_OBJ:.o=.d)

.PHONY: all test sanitize-test release bench bench-real install uninstall clean

all: $(TARGET)

$(TARGET): $(OBJ) $(EXTRA_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_TARGET): $(filter-out src/main.o,$(OBJ)) $(TEST_OBJ) $(EXTRA_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET)

sanitize-test:
	$(MAKE) clean
	$(MAKE) CC=clang EXTRA_CPPFLAGS="-DFCONCAT_LEAK_GUARD" CFLAGS="$(SANITIZER_FLAGS)" LDFLAGS="-fsanitize=address,undefined $(ALLOC_WRAP_LDFLAGS)" EXTRA_SRC="tests/leak_guard.c" $(TARGET) $(TEST_TARGET)
	$(SANITIZER_ENV) ./$(TEST_TARGET)

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O3 -DNDEBUG" all

bench: release
	@root="$${BENCH_ROOT:-$$HOME/projects}"; \
	iterations="$${BENCH_ITERATIONS:-7}"; \
	if [ ! -d "$$root" ]; then echo "BENCH_ROOT is not a directory: $$root" >&2; exit 1; fi; \
	echo "Benchmarking raw traversal $$root -> /dev/null ($$iterations runs after warmup)"; \
	./$(TARGET) "$$root" /dev/null >/dev/null; \
	i=1; \
	while [ "$$i" -le "$$iterations" ]; do \
		start_ns="$$(date +%s%N)"; \
		./$(TARGET) "$$root" /dev/null >/dev/null; \
		status="$$?"; \
		end_ns="$$(date +%s%N)"; \
		if [ "$$status" -ne 0 ]; then exit "$$status"; fi; \
		elapsed_ns="$$((end_ns - start_ns))"; \
		awk -v i="$$i" -v ns="$$elapsed_ns" 'BEGIN { printf "run %d %.3f\n", i, ns / 1000000000 }'; \
		i="$$((i + 1))"; \
	done

bench-real: release
	@root="$${BENCH_ROOT:-$$HOME/projects}"; \
	output="$${BENCH_OUTPUT:-$$HOME/fconcat-bench-real-output.txt}"; \
	iterations="$${BENCH_ITERATIONS:-3}"; \
	if [ ! -d "$$root" ]; then echo "BENCH_ROOT is not a directory: $$root" >&2; exit 1; fi; \
	echo "Benchmarking real output $$root -> $$output ($$iterations runs)"; \
	i=1; \
	while [ "$$i" -le "$$iterations" ]; do \
		rm -f "$$output"; \
		start_ns="$$(date +%s%N)"; \
		./$(TARGET) "$$root" "$$output" >/dev/null; \
		status="$$?"; \
		end_ns="$$(date +%s%N)"; \
		if [ "$$status" -ne 0 ]; then exit "$$status"; fi; \
		elapsed_ns="$$((end_ns - start_ns))"; \
		bytes="$$(wc -c < "$$output")"; \
		awk -v i="$$i" -v ns="$$elapsed_ns" -v bytes="$$bytes" 'BEGIN { printf "run %d %.3f bytes %s\n", i, ns / 1000000000, bytes }'; \
		i="$$((i + 1))"; \
	done; \
	if [ "$${BENCH_KEEP_OUTPUT:-0}" != "1" ]; then rm -f "$$output"; fi

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"

clean:
	rm -f $(TARGET) $(TEST_TARGET)
	find src tests -type f \( -name '*.o' -o -name '*.d' \) -delete

-include $(DEP)
