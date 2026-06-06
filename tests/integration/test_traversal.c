/**
 * @file test_traversal.c
 * @brief Integration tests for fconcat binary
 * 
 * Tests the compiled fconcat binary end-to-end:
 * - Basic file concatenation
 * - Empty directories
 * - Symlink detection and handling  
 * - Circular symlink protection
 * - Deep directory nesting
 * - Binary file filtering
 * - Pattern filtering
 */

#include "../unit/test_framework.h"
#include "../../src/server/server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* 
 * Disable format-truncation warnings for this file.
 * The test_root path is always short (/tmp/fconcat_integ_XXXXX, ~30 chars)
 * so the 512-byte buffers are more than sufficient. The compiler cannot
 * statically prove this, so we suppress the false positive warnings.
 * Note: -Wformat-truncation is GCC-specific, Clang doesn't recognize it.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/* Use smaller buffers for paths - test_root is short (/tmp/fconcat_XXXXX) */
#define TEST_PATH_MAX 512

/* =========================================================================
 * Test Helpers
 * ========================================================================= */

static char test_root[TEST_PATH_MAX] = {0};
static char fconcat_bin[TEST_PATH_MAX] = {0};
static int test_root_created = 0;

static void create_test_root(void)
{
    if (test_root_created) return;
    const char *base = getenv("TMPDIR");
    if (!base || base[0] == '\0')
        base = getenv("HOME");
    if (!base || base[0] == '\0')
        base = ".";
    int n = snprintf(test_root, sizeof(test_root), "%s/fconcat_integ_%d", base, getpid());
    if (n < 0 || (size_t)n >= sizeof(test_root)) {
        fprintf(stderr, "ERROR: test_root path too long\n");
        exit(1);
    }
    if (mkdir(test_root, 0755) != 0 && errno != EEXIST) {
        perror("mkdir test_root");
        exit(1);
    }
    test_root_created = 1;
}

static void cleanup_test_root(void)
{
    if (!test_root_created) return;
    char cmd[TEST_PATH_MAX + 32];
    int n = snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_root);
    if (n > 0 && (size_t)n < sizeof(cmd)) {
        int ret = system(cmd);
        (void)ret;
    }
    test_root_created = 0;
}

static int create_dir(const char *relpath)
{
    char path[TEST_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", test_root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    return mkdir(path, 0755);
}

static int create_file(const char *relpath, const char *content)
{
    char path[TEST_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", test_root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static int create_large_text_file(const char *relpath, size_t min_size)
{
    char path[TEST_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", test_root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    size_t written = 0;
    const char *chunk = "0123456789abcdef large text payload line\n";
    size_t chunk_len = strlen(chunk);
    while (written < min_size) {
        if (fwrite(chunk, 1, chunk_len, f) != chunk_len) {
            fclose(f);
            return -1;
        }
        written += chunk_len;
    }
    fputs("TAIL-MARKER\n", f);
    fclose(f);
    return 0;
}

static int create_binary_file(const char *relpath, size_t size)
{
    char path[TEST_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", test_root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    /* Write data with null bytes (binary indicator) */
    for (size_t i = 0; i < size; i++) {
        unsigned char byte = (i % 256 == 0) ? 0 : (unsigned char)(i % 256);
        fwrite(&byte, 1, 1, f);
    }
    fclose(f);
    return 0;
}

static int http_fire_and_close_local(int port, const char *target)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    char request[TEST_PATH_MAX + 256];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Connection: close\r\n\r\n",
                     target);
    if (n < 0 || (size_t)n >= sizeof(request)) {
        close(fd);
        return -1;
    }

    (void)send(fd, request, (size_t)n, 0);
    close(fd);
    return 0;
}

static int create_symlink_file(const char *target, const char *linkpath)
{
    char full_link[TEST_PATH_MAX];
    int n = snprintf(full_link, sizeof(full_link), "%s/%s", test_root, linkpath);
    if (n < 0 || (size_t)n >= sizeof(full_link)) return -1;
    return symlink(target, full_link);
}

static int set_permissions(const char *relpath, mode_t mode)
{
    char path[TEST_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", test_root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    return chmod(path, mode);
}

/**
 * Read contents of output file into buffer
 */
static int read_output_file(const char *filepath, char *buf, size_t bufsize)
{
    FILE *f = fopen(filepath, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, bufsize - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

/**
 * Get path for test output file
 */
static const char *get_output_path(void)
{
    static char output_path[TEST_PATH_MAX];
    snprintf(output_path, sizeof(output_path), "%s/output.txt", test_root);
    return output_path;
}

/**
 * Run fconcat and capture output
 * Returns exit code, stores output in buffer
 */
static int run_fconcat(char *output, size_t output_size, const char *args_fmt, ...)
{
    char args[1024];
    va_list ap;
    va_start(ap, args_fmt);
    vsnprintf(args, sizeof(args), args_fmt, ap);
    va_end(ap);
    
    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd), "%s %s 2>&1", fconcat_bin, args);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    
    size_t total = 0;
    if (output && output_size > 0) {
        output[0] = '\0';
        while (total < output_size - 1) {
            size_t rd = fread(output + total, 1, output_size - 1 - total, fp);
            if (rd == 0) break;
            total += rd;
        }
        output[total] = '\0';
    }
    
    int status = pclose(fp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_fconcat_env(char *output, size_t output_size, const char *env_prefix, const char *args_fmt, ...)
{
    char args[1024];
    va_list ap;
    va_start(ap, args_fmt);
    vsnprintf(args, sizeof(args), args_fmt, ap);
    va_end(ap);

    char cmd[2300];
    int n;
    if (env_prefix && env_prefix[0] != '\0') {
        n = snprintf(cmd, sizeof(cmd), "%s %s %s 2>&1", env_prefix, fconcat_bin, args);
    } else {
        n = snprintf(cmd, sizeof(cmd), "%s %s 2>&1", fconcat_bin, args);
    }
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    size_t total = 0;
    if (output && output_size > 0) {
        output[0] = '\0';
        while (total < output_size - 1) {
            size_t rd = fread(output + total, 1, output_size - 1 - total, fp);
            if (rd == 0) break;
            total += rd;
        }
        output[total] = '\0';
    }

    int status = pclose(fp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_shell_capture(char *output, size_t output_size, const char *cmd_fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, cmd_fmt);
    int n = vsnprintf(cmd, sizeof(cmd), cmd_fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    size_t total = 0;
    if (output && output_size > 0) {
        output[0] = '\0';
        while (total < output_size - 1) {
            size_t rd = fread(output + total, 1, output_size - 1 - total, fp);
            if (rd == 0) break;
            total += rd;
        }
        output[total] = '\0';
    }

    int status = pclose(fp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/**
 * Check if output contains a substring
 */
static int output_contains(const char *output, const char *needle)
{
    return output && needle && strstr(output, needle) != NULL;
}

static int http_get_local(int port, const char *target, char *output, size_t output_size)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    char request[TEST_PATH_MAX + 256];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Connection: close\r\n\r\n",
                     target);
    if (n < 0 || (size_t)n >= sizeof(request)) {
        close(fd);
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t wr = send(fd, request + sent, (size_t)n - sent, 0);
        if (wr < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        sent += (size_t)wr;
    }

    size_t total = 0;
    if (output && output_size > 0)
        output[0] = '\0';

    while (output && total + 1 < output_size) {
        ssize_t rd = recv(fd, output + total, output_size - total - 1, 0);
        if (rd < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (rd == 0)
            break;
        total += (size_t)rd;
    }

    if (output && output_size > 0)
        output[total] = '\0';

    close(fd);
    return 0;
}

static pid_t start_fconcat_server(const char *root, int port)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;

    char listen[64];
    snprintf(listen, sizeof(listen), "127.0.0.1:%d", port);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    execl(fconcat_bin, fconcat_bin,
          "--serve",
          "--listen", listen,
          "--allow-root", root,
          "--workers", "1",
          "--queue", "4",
          (char *)NULL);
    _exit(127);
}

static int stop_fconcat_server(pid_t pid)
{
    if (pid <= 0)
        return -1;

    kill(pid, SIGTERM);
    for (int i = 0; i < 40; i++) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        usleep(100000);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static int wait_for_server_ready(int port)
{
    char response[1024];
    for (int i = 0; i < 50; i++) {
        if (http_get_local(port, "/healthz", response, sizeof(response)) == 0 &&
            output_contains(response, "HTTP/1.1 200 OK") &&
            output_contains(response, "ok")) {
            return 0;
        }
        usleep(100000);
    }
    return -1;
}

/**
 * Count occurrences of pattern in output
 */
static int count_occurrences(const char *output, const char *pattern)
{
    if (!output || !pattern) return 0;
    int count = 0;
    const char *p = output;
    size_t len = strlen(pattern);
    while ((p = strstr(p, pattern)) != NULL) {
        count++;
        p += len;
    }
    return count;
}

/* =========================================================================
 * Basic Integration Tests
 * ========================================================================= */

TEST(integ_single_file)
{
    create_test_root();
    create_dir("single");
    create_file("single/hello.txt", "Hello, World!");
    
    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/single", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "hello.txt"));
    ASSERT_TRUE(output_contains(content, "Hello, World!"));
    
    return 0;
}

TEST(integ_empty_directory)
{
    create_test_root();
    create_dir("empty");
    
    char cmdout[1024];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/empty", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    /* Empty directory should not cause an error */
    ASSERT_EQ(0, exit_code);
    
    return 0;
}

TEST(integ_nested_directories)
{
    create_test_root();
    create_dir("nested");
    create_dir("nested/level1");
    create_dir("nested/level1/level2");
    create_file("nested/root.txt", "root content");
    create_file("nested/level1/l1.txt", "level1 content");
    create_file("nested/level1/level2/l2.txt", "level2 content");
    
    char cmdout[1024];
    char content[16384];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/nested", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "root.txt"));
    ASSERT_TRUE(output_contains(content, "l1.txt"));
    ASSERT_TRUE(output_contains(content, "l2.txt"));
    ASSERT_TRUE(output_contains(content, "root content"));
    ASSERT_TRUE(output_contains(content, "level1 content"));
    ASSERT_TRUE(output_contains(content, "level2 content"));
    
    return 0;
}

TEST(integ_multiple_files)
{
    create_test_root();
    create_dir("multi");
    create_file("multi/file1.txt", "content1");
    create_file("multi/file2.txt", "content2");
    create_file("multi/file3.txt", "content3");
    
    char cmdout[1024];
    char content[16384];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/multi", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    /* All three files should appear in output */
    int file_count = count_occurrences(content, "File:");
    ASSERT_TRUE(file_count >= 3);
    
    return 0;
}

TEST(integ_single_file_golden_output)
{
    create_test_root();
    create_dir("golden");
    create_file("golden/a.txt", "alpha\n");

    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/golden", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());

    const char *expected =
        "Directory Structure:\n"
        "==================\n"
        "\n"
        "FILE a.txt\n"
        "\n"
        "File Contents:\n"
        "=============\n"
        "\n"
        "// File: a.txt\n"
        "alpha\n"
        "\n"
        "\n";

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_STR_EQ(expected, content);

    return 0;
}

TEST(integ_show_size_golden_output)
{
    create_test_root();
    create_dir("golden-size");
    create_file("golden-size/a.txt", "abc\n");

    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/golden-size", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s' --show-size", input_path, get_output_path());

    const char *expected =
        "Directory Structure:\n"
        "==================\n"
        "\n"
        "FILE [1 KB] a.txt\n"
        "\n"
        "File Contents:\n"
        "=============\n"
        "\n"
        "// File: a.txt\n"
        "abc\n"
        "\n"
        "\n";

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_STR_EQ(expected, content);

    return 0;
}

TEST(integ_sorted_golden_output)
{
    create_test_root();
    create_dir("golden-sorted");
    create_file("golden-sorted/b.txt", "bravo\n");
    create_file("golden-sorted/a.txt", "alpha\n");

    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/golden-sorted", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());

    const char *expected =
        "Directory Structure:\n"
        "==================\n"
        "\n"
        "FILE a.txt\n"
        "FILE b.txt\n"
        "\n"
        "File Contents:\n"
        "=============\n"
        "\n"
        "// File: a.txt\n"
        "alpha\n"
        "\n"
        "\n"
        "// File: b.txt\n"
        "bravo\n"
        "\n"
        "\n";

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_STR_EQ(expected, content);

    return 0;
}

TEST(integ_output_file_self_exclusion)
{
    create_test_root();
    create_dir("self-exclude");
    create_file("self-exclude/source.txt", "source content");

    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    char output_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/self-exclude", test_root);
    snprintf(output_path, sizeof(output_path), "%s/self-exclude/out.txt", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, output_path);

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(output_path, content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "source.txt"));
    ASSERT_FALSE(output_contains(content, "out.txt"));

    return 0;
}

/* =========================================================================
 * Symlink Tests
 * ========================================================================= */

TEST(integ_symlink_skip_default)
{
    create_test_root();
    create_dir("symtest");
    create_file("symtest/target.txt", "I am the target");
    create_symlink_file("target.txt", "symtest/link.txt");
    
    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/symtest", test_root);
    
    /* Default behavior should skip symlinks */
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "target.txt"));
    ASSERT_FALSE(output_contains(content, "link.txt"));
    ASSERT_EQ(1, count_occurrences(content, "I am the target"));
    
    return 0;
}

TEST(integ_circular_symlink_no_hang)
{
    create_test_root();
    create_dir("circular");
    create_dir("circular/a");
    create_dir("circular/a/b");
    
    /* Create circular symlink: circular/a/b/back -> ../../a (points to circular/a) */
    create_symlink_file("../../a", "circular/a/b/back");
    create_file("circular/a/file.txt", "test content");
    
    char cmdout[1024];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/circular", test_root);
    
    /* This should NOT hang - circular symlink protection should prevent infinite loop */
    /* Options must come after positional args */
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), 
                                "'%s' '%s' --symlinks follow", input_path, get_output_path());
    
    /* Should complete without hanging (test will timeout if it hangs) */
    ASSERT_EQ(0, exit_code);
    
    return 0;
}

TEST(integ_broken_symlink)
{
    create_test_root();
    create_dir("broken");
    create_file("broken/good.txt", "good file");
    create_symlink_file("nonexistent.txt", "broken/bad_link.txt");
    
    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/broken", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    /* Should handle broken symlinks gracefully */
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "good.txt"));
    
    return 0;
}

TEST(integ_symlink_placeholder)
{
    create_test_root();
    create_dir("symplaceholder");
    create_file("symplaceholder/target.txt", "target content");
    create_symlink_file("target.txt", "symplaceholder/link.txt");

    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/symplaceholder", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout),
                                "'%s' '%s' --symlinks placeholder", input_path, get_output_path());

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "link.txt"));
    ASSERT_TRUE(output_contains(content, "Symbolic link to: target.txt"));

    return 0;
}

TEST(integ_symlink_include)
{
    create_test_root();
    create_dir("syminclude");
    create_file("syminclude/target.txt", "included target");
    create_symlink_file("target.txt", "syminclude/link.txt");

    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/syminclude", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout),
                                "'%s' '%s' --symlinks include", input_path, get_output_path());

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "target.txt"));
    ASSERT_TRUE(output_contains(content, "link.txt"));
    ASSERT_EQ(2, count_occurrences(content, "included target"));

    return 0;
}

/* =========================================================================
 * Binary File Tests
 * ========================================================================= */

TEST(integ_binary_file_detection)
{
    create_test_root();
    create_dir("bintest");
    create_file("bintest/text.txt", "This is plain text");
    create_binary_file("bintest/binary.bin", 256);
    
    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/bintest", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    /* Text file should be included */
    ASSERT_TRUE(output_contains(content, "text.txt"));
    ASSERT_TRUE(output_contains(content, "This is plain text"));
    ASSERT_FALSE(output_contains(content, "binary.bin"));
    ASSERT_FALSE(output_contains(content, "[Binary file content not displayed]"));
    
    return 0;
}

TEST(integ_binary_placeholder)
{
    create_test_root();
    create_dir("binplaceholder");
    create_binary_file("binplaceholder/binary.bin", 256);

    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/binplaceholder", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout),
                                "'%s' '%s' --binary-placeholder", input_path, get_output_path());

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "binary.bin"));
    ASSERT_TRUE(output_contains(content, "[Binary file content not displayed]"));

    return 0;
}

TEST(integ_large_text_file_streaming)
{
    create_test_root();
    create_dir("large");
    create_large_text_file("large/big.txt", 200 * 1024);

    char cmdout[1024];
    char content[260 * 1024];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/large", test_root);

    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());

    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, 260 * 1024));
    ASSERT_TRUE(output_contains(content, "big.txt"));
    ASSERT_TRUE(output_contains(content, "TAIL-MARKER"));

    return 0;
}

TEST(integ_prefix_cache_disabled_matches_default)
{
    create_test_root();
    create_dir("prefix-cache");
    create_file("prefix-cache/a.txt", "alpha\n");
    create_large_text_file("prefix-cache/big.txt", 80 * 1024);

    char cmdout[2048];
    char default_content[140 * 1024];
    char disabled_content[140 * 1024];

    char input_path[TEST_PATH_MAX];
    char output_default[TEST_PATH_MAX];
    char output_disabled[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/prefix-cache", test_root);
    snprintf(output_default, sizeof(output_default), "%s/prefix-default.txt", test_root);
    snprintf(output_disabled, sizeof(output_disabled), "%s/prefix-disabled.txt", test_root);

    int exit_default = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, output_default);
    int exit_disabled = run_fconcat_env(cmdout, sizeof(cmdout),
                                        "FCONCAT_PREFIX_CACHE_MB=0 FCONCAT_PREFIX_FILE_KB=1",
                                        "'%s' '%s'", input_path, output_disabled);

    ASSERT_EQ(0, exit_default);
    ASSERT_EQ(0, exit_disabled);
    ASSERT_EQ(0, read_output_file(output_default, default_content, 140 * 1024));
    ASSERT_EQ(0, read_output_file(output_disabled, disabled_content, 140 * 1024));
    ASSERT_STR_EQ(default_content, disabled_content);

    return 0;
}

/* =========================================================================
 * Filter Pattern Tests
 * ========================================================================= */

TEST(integ_include_pattern)
{
    create_test_root();
    create_dir("patterns");
    create_file("patterns/file.txt", "text file");
    create_file("patterns/file.md", "markdown file");
    create_file("patterns/file.c", "c file");
    
    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/patterns", test_root);
    
    /* Only include .txt files - options must come after positional args */
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), 
                                "'%s' '%s' --include '*.txt'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "file.txt"));
    /* Other files should not appear in content */
    ASSERT_FALSE(output_contains(content, "markdown file"));
    ASSERT_FALSE(output_contains(content, "c file"));
    
    return 0;
}

TEST(integ_exclude_pattern)
{
    create_test_root();
    create_dir("exclude");
    create_dir("exclude/.git");
    create_file("exclude/main.c", "main code");
    create_file("exclude/.git/config", "git config");
    
    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/exclude", test_root);
    
    /* Exclude .git directory - options must come after positional args */
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), 
                                "'%s' '%s' --exclude '.git'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "main.c"));
    /* .git files should be excluded */
    ASSERT_FALSE(output_contains(content, "git config"));
    
    return 0;
}

/* =========================================================================
 * Permission Tests
 * ========================================================================= */

TEST(integ_permission_denied)
{
    /* Skip if running as root (root can read anything) */
    if (getuid() == 0) {
        return 0;  /* Skip test */
    }
    
    create_test_root();
    create_dir("perms");
    create_dir("perms/readable");
    create_dir("perms/unreadable");
    create_file("perms/readable/ok.txt", "readable content");
    create_file("perms/unreadable/secret.txt", "secret content");
    set_permissions("perms/unreadable", 0000);
    
    char cmdout[1024];
    char content[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/perms", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    /* Should complete despite permission errors */
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "readable content"));
    /* Secret content should NOT appear (permission denied) */
    ASSERT_FALSE(output_contains(content, "secret content"));
    
    /* Restore permissions for cleanup */
    set_permissions("perms/unreadable", 0755);
    
    return 0;
}

/* =========================================================================
 * Deep Nesting Test
 * ========================================================================= */

TEST(integ_deep_nesting)
{
    create_test_root();
    create_dir("deep");
    
    /* Create 30 levels of nesting */
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof(path), "deep");
    for (int i = 0; i < 30; i++) {
        char newpath[TEST_PATH_MAX];
        int n = snprintf(newpath, sizeof(newpath), "%s/L%d", path, i);
        if (n < 0 || (size_t)n >= sizeof(newpath)) break;
        create_dir(newpath);
        snprintf(path, sizeof(path), "%s", newpath);
    }
    
    /* Create a file at the deepest level */
    char filepath[TEST_PATH_MAX];
    int n = snprintf(filepath, sizeof(filepath), "%s/deep.txt", path);
    if (n > 0 && (size_t)n < sizeof(filepath)) {
        create_file(filepath, "deep file content");
    }
    
    char cmdout[1024];
    char content[16384];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/deep", test_root);
    
    int exit_code = run_fconcat(cmdout, sizeof(cmdout), "'%s' '%s'", input_path, get_output_path());
    
    ASSERT_EQ(0, exit_code);
    ASSERT_EQ(0, read_output_file(get_output_path(), content, sizeof(content)));
    ASSERT_TRUE(output_contains(content, "deep.txt"));
    ASSERT_TRUE(output_contains(content, "deep file content"));
    
    return 0;
}

/* =========================================================================
 * Command Line Option Tests
 * ========================================================================= */

TEST(integ_help_option)
{
    char output[8192];
    int exit_code = run_fconcat(output, sizeof(output), "--help");
    
    /* --help prints usage and returns 0 (success) */
    ASSERT_EQ(0, exit_code);
    ASSERT_TRUE(output_contains(output, "Usage") || output_contains(output, "usage"));
    
    return 0;
}

TEST(integ_version_option)
{
    char output[8192];
    int exit_code = run_fconcat(output, sizeof(output), "--version");
    
    /* --version prints version and returns 0 (success) */
    ASSERT_EQ(0, exit_code);
    /* Should display some version info */
    ASSERT_TRUE(strlen(output) > 0);
    
    return 0;
}

TEST(integ_nonexistent_directory)
{
    create_test_root();
    char output[8192];
    int exit_code = run_fconcat(output, sizeof(output), 
                                "/nonexistent/path '%s'", get_output_path());
    
    ASSERT_NE(0, exit_code);
    ASSERT_TRUE(output_contains(output, "Invalid input directory"));
    
    return 0;
}

TEST(integ_removed_format_option_is_rejected)
{
    create_test_root();
    create_dir("removed-format");
    create_file("removed-format/file.txt", "content");

    char output[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/removed-format", test_root);

    int exit_code = run_fconcat(output, sizeof(output),
                                "'%s' '%s' --format json", input_path, get_output_path());

    ASSERT_NE(0, exit_code);
    ASSERT_TRUE(output_contains(output, "Invalid arguments"));
    
    return 0;
}

TEST(integ_removed_plugin_option_is_rejected)
{
    create_test_root();
    create_dir("removed-plugin");
    create_file("removed-plugin/file.txt", "content");

    char output[8192];
    char input_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/removed-plugin", test_root);

    int exit_code = run_fconcat(output, sizeof(output),
                                "'%s' '%s' --plugin ./old.so", input_path, get_output_path());

    ASSERT_NE(0, exit_code);
    ASSERT_TRUE(output_contains(output, "Invalid arguments"));
    
    return 0;
}

TEST(integ_benchmark_script_check)
{
    create_test_root();
    create_dir("bench-check");

    char root[TEST_PATH_MAX];
    snprintf(root, sizeof(root), "%s/bench-check", test_root);

    char output[8192];
    int ok = run_shell_capture(output, sizeof(output),
                               "BENCH_BIN='./fconcat' BENCH_ROOT='%s' sh scripts/bench.sh check 2>&1",
                               root);
    ASSERT_EQ(0, ok);
    ASSERT_TRUE(output_contains(output, "mode=check"));
    ASSERT_TRUE(output_contains(output, "BENCH_CHECK: OK"));

    int bad = run_shell_capture(output, sizeof(output),
                                "BENCH_BIN='./fconcat' BENCH_ROOT='%s/missing' sh scripts/bench.sh check 2>&1",
                                root);
    ASSERT_NE(0, bad);
    ASSERT_TRUE(output_contains(output, "BENCH_ROOT is not a directory"));

    return 0;
}

TEST(integ_oom_cleanup_under_leak_guard)
{
#ifndef FCONCAT_LEAK_GUARD
    return 0;
#else
    create_test_root();
    create_dir("oom");
    create_file("oom/a.txt", "oom path\n");

    char input_path[TEST_PATH_MAX];
    char output_path[TEST_PATH_MAX];
    snprintf(input_path, sizeof(input_path), "%s/oom", test_root);
    snprintf(output_path, sizeof(output_path), "%s/oom-out.txt", test_root);

    for (int fail_after = 0; fail_after <= 64; fail_after++) {
        char env[128];
        snprintf(env, sizeof(env), "LEAK_GUARD_FAIL_AFTER=%d", fail_after);

        char output[8192];
        int exit_code = run_fconcat_env(output, sizeof(output), env,
                                        "'%s' '%s'", input_path, output_path);
        ASSERT_NE(23, exit_code);
        ASSERT_FALSE(output_contains(output, "LEAK_GUARD: FAILED"));
    }

    const int late_fail_points[] = {96, 128, 192};
    for (size_t i = 0; i < sizeof(late_fail_points) / sizeof(late_fail_points[0]); i++) {
        char env[128];
        snprintf(env, sizeof(env), "LEAK_GUARD_FAIL_AFTER=%d", late_fail_points[i]);

        char output[8192];
        int exit_code = run_fconcat_env(output, sizeof(output), env,
                                        "'%s' '%s'", input_path, output_path);
        ASSERT_NE(23, exit_code);
        ASSERT_FALSE(output_contains(output, "LEAK_GUARD: FAILED"));
    }

    return 0;
#endif
}

TEST(integ_server_health_and_concat_stream)
{
    create_test_root();
    create_dir("server-root");
    create_file("server-root/a.txt", "server content");
    create_file("server-root/b.c", "filtered out");

    char root[TEST_PATH_MAX];
    snprintf(root, sizeof(root), "%s/server-root", test_root);

    int port = 18000 + (getpid() % 20000);
    pid_t pid = start_fconcat_server(root, port);
    ASSERT_TRUE(pid > 0);

    if (wait_for_server_ready(port) != 0) {
        (void)stop_fconcat_server(pid);
        ASSERT_TRUE(0);
    }

    char bad_target[TEST_PATH_MAX + 128];
    snprintf(bad_target, sizeof(bad_target), "/concat?root=%s&include=*.txt&unknown=x", root);
    char bad_response[8192];
    ASSERT_EQ(0, http_get_local(port, bad_target, bad_response, sizeof(bad_response)));
    ASSERT_TRUE(output_contains(bad_response, "HTTP/1.1 400 Bad Request"));

    char target[TEST_PATH_MAX + 128];
    snprintf(target, sizeof(target), "/concat?root=%s&include=*.txt", root);

    char response[65536];
    int result = http_get_local(port, target, response, sizeof(response));
    int server_exit = stop_fconcat_server(pid);

    ASSERT_EQ(0, result);
    ASSERT_EQ(0, server_exit);
    ASSERT_TRUE(output_contains(response, "HTTP/1.1 200 OK"));
    ASSERT_TRUE(output_contains(response, "Transfer-Encoding: chunked"));
    ASSERT_TRUE(output_contains(response, "a.txt"));
    ASSERT_TRUE(output_contains(response, "server content"));
    ASSERT_FALSE(output_contains(response, "filtered out"));

    return 0;
}

TEST(integ_server_malformed_query_cleanup)
{
    create_test_root();
    create_dir("server-malformed");
    create_file("server-malformed/a.txt", "server content");

    char root[TEST_PATH_MAX];
    snprintf(root, sizeof(root), "%s/server-malformed", test_root);

    int port = 18500 + (getpid() % 20000);
    pid_t pid = start_fconcat_server(root, port);
    ASSERT_TRUE(pid > 0);

    if (wait_for_server_ready(port) != 0) {
        (void)stop_fconcat_server(pid);
        ASSERT_TRUE(0);
    }

    char response[8192];
    int malformed_result = http_get_local(port, "/concat?root=%zz", response, sizeof(response));
    int malformed_is_bad_request = output_contains(response, "HTTP/1.1 400 Bad Request");
    int truncated_result = http_get_local(port, "/concat?root=%", response, sizeof(response));
    int truncated_is_bad_request = output_contains(response, "HTTP/1.1 400 Bad Request");

    int health_result = http_get_local(port, "/healthz", response, sizeof(response));
    int health_is_ok = output_contains(response, "HTTP/1.1 200 OK");
    int server_exit = stop_fconcat_server(pid);

    ASSERT_EQ(0, malformed_result);
    ASSERT_TRUE(malformed_is_bad_request);
    ASSERT_EQ(0, truncated_result);
    ASSERT_TRUE(truncated_is_bad_request);
    ASSERT_EQ(0, health_result);
    ASSERT_EQ(0, server_exit);
    ASSERT_TRUE(health_is_ok);

    return 0;
}

TEST(integ_server_denied_root)
{
    create_test_root();
    create_dir("server-allowed");
    create_dir("server-denied");
    create_file("server-denied/secret.txt", "secret");

    char allowed[TEST_PATH_MAX];
    char denied[TEST_PATH_MAX];
    snprintf(allowed, sizeof(allowed), "%s/server-allowed", test_root);
    snprintf(denied, sizeof(denied), "%s/server-denied", test_root);

    int port = 19000 + (getpid() % 20000);
    pid_t pid = start_fconcat_server(allowed, port);
    ASSERT_TRUE(pid > 0);

    if (wait_for_server_ready(port) != 0) {
        (void)stop_fconcat_server(pid);
        ASSERT_TRUE(0);
    }

    char target[TEST_PATH_MAX + 128];
    snprintf(target, sizeof(target), "/concat?root=%s", denied);
    char response[8192];
    int result = http_get_local(port, target, response, sizeof(response));
    int server_exit = stop_fconcat_server(pid);

    ASSERT_EQ(0, result);
    ASSERT_EQ(0, server_exit);
    ASSERT_TRUE(output_contains(response, "HTTP/1.1 403 Forbidden"));

    return 0;
}

TEST(integ_server_client_disconnect_cleanup)
{
    create_test_root();
    create_dir("server-disconnect");
    create_large_text_file("server-disconnect/big.txt", 200 * 1024);

    char root[TEST_PATH_MAX];
    snprintf(root, sizeof(root), "%s/server-disconnect", test_root);

    int port = 20000 + (getpid() % 20000);
    pid_t pid = start_fconcat_server(root, port);
    ASSERT_TRUE(pid > 0);

    if (wait_for_server_ready(port) != 0) {
        (void)stop_fconcat_server(pid);
        ASSERT_TRUE(0);
    }

    char target[TEST_PATH_MAX + 128];
    snprintf(target, sizeof(target), "/concat?root=%s", root);
    ASSERT_EQ(0, http_fire_and_close_local(port, target));
    usleep(200000);

    char response[1024];
    int health = http_get_local(port, "/healthz", response, sizeof(response));
    int server_exit = stop_fconcat_server(pid);

    ASSERT_EQ(0, health);
    ASSERT_EQ(0, server_exit);
    ASSERT_TRUE(output_contains(response, "HTTP/1.1 200 OK"));

    return 0;
}

TEST(integ_server_rejects_invalid_config)
{
    ResolvedConfig config;
    memset(&config, 0, sizeof(config));

    ASSERT_EQ(-1, server_run(NULL, NULL, NULL));
    ASSERT_EQ(-1, server_run(&config, NULL, NULL));

    char *roots[] = {"/path/that/should/not/exist/fconcat"};
    config.listen_host = "127.0.0.1";
    config.listen_port = 18080;
    config.allow_roots = NULL;
    config.allow_root_count = 1;
    config.server_workers = 1;
    config.server_queue_size = 1;
    ASSERT_EQ(-1, server_run(&config, NULL, NULL));

    config.allow_roots = roots;
    config.allow_root_count = MAX_ALLOW_ROOTS + 1;
    ASSERT_EQ(-1, server_run(&config, NULL, NULL));

    config.allow_root_count = 1;
    config.server_workers = 0;
    ASSERT_EQ(-1, server_run(&config, NULL, NULL));

    config.server_workers = 1;
    config.server_queue_size = 0;
    ASSERT_EQ(-1, server_run(&config, NULL, NULL));

    config.server_queue_size = 1;
    ASSERT_EQ(-1, server_run(&config, NULL, NULL));

    return 0;
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

int test_traversal_main(void)
{
    /* Determine fconcat binary location */
    char cwd[TEST_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "ERROR: Cannot get current directory\n");
        return 1;
    }
    snprintf(fconcat_bin, sizeof(fconcat_bin), "%s/fconcat", cwd);
    
    /* Check if binary exists */
    if (access(fconcat_bin, X_OK) != 0) {
        fprintf(stderr, "ERROR: fconcat binary not found at %s\n", fconcat_bin);
        fprintf(stderr, "Please run 'make' before running integration tests\n");
        return 1;
    }
    
    /* Reset counters for this test suite */
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
    
    TEST_SUITE_BEGIN("Basic Integration");
    RUN_TEST(integ_single_file);
    RUN_TEST(integ_empty_directory);
    RUN_TEST(integ_nested_directories);
    RUN_TEST(integ_multiple_files);
    RUN_TEST(integ_single_file_golden_output);
    RUN_TEST(integ_show_size_golden_output);
    RUN_TEST(integ_sorted_golden_output);
    RUN_TEST(integ_output_file_self_exclusion);
    
    TEST_SUITE_BEGIN("Symlink Handling");
    RUN_TEST(integ_symlink_skip_default);
    RUN_TEST(integ_circular_symlink_no_hang);
    RUN_TEST(integ_broken_symlink);
    RUN_TEST(integ_symlink_placeholder);
    RUN_TEST(integ_symlink_include);
    
    TEST_SUITE_BEGIN("Binary File Detection");
    RUN_TEST(integ_binary_file_detection);
    RUN_TEST(integ_binary_placeholder);
    RUN_TEST(integ_large_text_file_streaming);
    RUN_TEST(integ_prefix_cache_disabled_matches_default);
    
    TEST_SUITE_BEGIN("Filter Patterns");
    RUN_TEST(integ_include_pattern);
    RUN_TEST(integ_exclude_pattern);
    
    TEST_SUITE_BEGIN("Permission Handling");
    RUN_TEST(integ_permission_denied);
    
    TEST_SUITE_BEGIN("Deep Nesting");
    RUN_TEST(integ_deep_nesting);
    
    TEST_SUITE_BEGIN("Command Line Options");
    RUN_TEST(integ_help_option);
    RUN_TEST(integ_version_option);
    RUN_TEST(integ_nonexistent_directory);
    RUN_TEST(integ_removed_format_option_is_rejected);
    RUN_TEST(integ_removed_plugin_option_is_rejected);
    RUN_TEST(integ_benchmark_script_check);
    RUN_TEST(integ_oom_cleanup_under_leak_guard);
    RUN_TEST(integ_server_health_and_concat_stream);
    RUN_TEST(integ_server_malformed_query_cleanup);
    RUN_TEST(integ_server_denied_root);
    RUN_TEST(integ_server_client_disconnect_cleanup);
    RUN_TEST(integ_server_rejects_invalid_config);
    
    TEST_SUMMARY();
    
    /* Cleanup */
    cleanup_test_root();
    
    return TEST_EXIT_CODE();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
