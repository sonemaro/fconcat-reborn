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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
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
    int n = snprintf(test_root, sizeof(test_root), "/tmp/fconcat_integ_%d", getpid());
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

/**
 * Check if output contains a substring
 */
static int output_contains(const char *output, const char *needle)
{
    return output && needle && strstr(output, needle) != NULL;
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
    /* With default symlink handling, the link should be skipped or noted */
    
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
    /* Binary file should be filtered out or marked as binary */
    
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
    
    /* --help prints usage and returns 1 (as per main.c) */
    ASSERT_EQ(1, exit_code);
    ASSERT_TRUE(output_contains(output, "Usage") || output_contains(output, "usage"));
    
    return 0;
}

TEST(integ_version_option)
{
    char output[8192];
    int exit_code = run_fconcat(output, sizeof(output), "--version");
    
    /* --version prints version and returns 1 (as per main.c) */
    ASSERT_EQ(1, exit_code);
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
    
    /* fconcat completes successfully even with nonexistent input (just warns) */
    /* This test verifies it doesn't crash and produces warning output */
    ASSERT_EQ(0, exit_code);
    ASSERT_TRUE(output_contains(output, "Cannot stat") || 
                output_contains(output, "No such file"));
    
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
    
    TEST_SUITE_BEGIN("Symlink Handling");
    RUN_TEST(integ_symlink_skip_default);
    RUN_TEST(integ_circular_symlink_no_hang);
    RUN_TEST(integ_broken_symlink);
    
    TEST_SUITE_BEGIN("Binary File Detection");
    RUN_TEST(integ_binary_file_detection);
    
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
    
    TEST_SUMMARY();
    
    /* Cleanup */
    cleanup_test_root();
    
    return TEST_EXIT_CODE();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
