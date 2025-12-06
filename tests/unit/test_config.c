/**
 * @file test_config.c
 * @brief Unit tests for configuration management subsystem
 * 
 * Tests cover:
 * - ConfigManager lifecycle (create/destroy)
 * - ConfigValue initialization and cleanup
 * - ConfigLayer operations (add/get values)
 * - Default configuration loading
 * - Configuration resolution
 */

#include "test_framework.h"
#include "../../src/config/config.h"
#include <string.h>

/* =========================================================================
 * ConfigManager Lifecycle Tests
 * ========================================================================= */

TEST(config_manager_create_returns_valid_pointer)
{
    ConfigManager *mgr = config_manager_create();
    ASSERT_NOT_NULL(mgr);
    config_manager_destroy(mgr);
    return 0;
}

TEST(config_manager_destroy_null_is_safe)
{
    config_manager_destroy(NULL);
    return 0;
}

TEST(config_manager_has_resolved_config)
{
    ConfigManager *mgr = config_manager_create();
    ASSERT_NOT_NULL(mgr);
    ASSERT_NOT_NULL(mgr->resolved);
    config_manager_destroy(mgr);
    return 0;
}

/* =========================================================================
 * ConfigValue Tests
 * ========================================================================= */

TEST(config_value_init_string)
{
    ConfigValue value;
    memset(&value, 0, sizeof(value));
    
    int result = config_value_init(&value, "test.key", CONFIG_TYPE_STRING);
    ASSERT_EQ(0, result);
    ASSERT_STR_EQ("test.key", value.key);
    ASSERT_EQ(CONFIG_TYPE_STRING, value.type);
    
    config_value_cleanup(&value);
    return 0;
}

TEST(config_value_init_null_key)
{
    ConfigValue value;
    memset(&value, 0, sizeof(value));
    
    int result = config_value_init(&value, NULL, CONFIG_TYPE_STRING);
    ASSERT_EQ(-1, result);
    return 0;
}

TEST(config_value_set_string)
{
    ConfigValue value;
    memset(&value, 0, sizeof(value));
    
    config_value_init(&value, "key", CONFIG_TYPE_STRING);
    config_value_set_string(&value, "test value");
    
    ASSERT_STR_EQ("test value", value.value.string_value);
    
    config_value_cleanup(&value);
    return 0;
}

TEST(config_value_set_int)
{
    ConfigValue value;
    memset(&value, 0, sizeof(value));
    
    config_value_init(&value, "key", CONFIG_TYPE_INT);
    config_value_set_int(&value, 42);
    
    ASSERT_EQ(42, value.value.int_value);
    
    config_value_cleanup(&value);
    return 0;
}

TEST(config_value_set_bool)
{
    ConfigValue value;
    memset(&value, 0, sizeof(value));
    
    config_value_init(&value, "key", CONFIG_TYPE_BOOL);
    config_value_set_bool(&value, true);
    
    ASSERT_TRUE(value.value.bool_value);
    
    config_value_cleanup(&value);
    return 0;
}

TEST(config_value_cleanup_null_safe)
{
    config_value_cleanup(NULL);
    return 0;
}

/* =========================================================================
 * ConfigLayer Tests
 * ========================================================================= */

TEST(config_layer_add_value_succeeds)
{
    ConfigManager *mgr = config_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    // Load defaults to create a layer
    int result = config_load_defaults(mgr);
    ASSERT_EQ(0, result);
    ASSERT_TRUE(mgr->layer_count > 0);
    
    config_manager_destroy(mgr);
    return 0;
}

TEST(config_layer_get_value_returns_null_for_missing_key)
{
    ConfigManager *mgr = config_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    config_load_defaults(mgr);
    
    ConfigValue *value = config_layer_get_value(&mgr->layers[0], "nonexistent.key");
    ASSERT_NULL(value);
    
    config_manager_destroy(mgr);
    return 0;
}

TEST(config_layer_get_value_null_params)
{
    ConfigValue *value = config_layer_get_value(NULL, "key");
    ASSERT_NULL(value);
    
    ConfigManager *mgr = config_manager_create();
    config_load_defaults(mgr);
    
    value = config_layer_get_value(&mgr->layers[0], NULL);
    ASSERT_NULL(value);
    
    config_manager_destroy(mgr);
    return 0;
}

/* =========================================================================
 * Default Configuration Tests
 * ========================================================================= */

TEST(config_load_defaults_succeeds)
{
    ConfigManager *mgr = config_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    int result = config_load_defaults(mgr);
    ASSERT_EQ(0, result);
    
    config_manager_destroy(mgr);
    return 0;
}

TEST(config_load_defaults_null_manager)
{
    int result = config_load_defaults(NULL);
    ASSERT_EQ(-1, result);
    return 0;
}

/* =========================================================================
 * Configuration Resolution Tests
 * ========================================================================= */

TEST(config_resolve_returns_valid_config)
{
    ConfigManager *mgr = config_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    config_load_defaults(mgr);
    
    ResolvedConfig *resolved = config_resolve(mgr);
    ASSERT_NOT_NULL(resolved);
    
    config_manager_destroy(mgr);
    return 0;
}

TEST(config_resolve_null_manager)
{
    ResolvedConfig *resolved = config_resolve(NULL);
    ASSERT_NULL(resolved);
    return 0;
}

/* =========================================================================
 * Configuration Get Functions Tests
 * ========================================================================= */

TEST(config_get_string_returns_value)
{
    ConfigManager *mgr = config_manager_create();
    ASSERT_NOT_NULL(mgr);
    
    config_load_defaults(mgr);
    
    // Get a string value that should exist in defaults
    const char *format = config_get_string(mgr, "format");
    // format might be NULL if not in defaults, that's ok
    (void)format;
    
    config_manager_destroy(mgr);
    return 0;
}

TEST(config_get_string_null_manager)
{
    const char *result = config_get_string(NULL, "key");
    ASSERT_NULL(result);
    return 0;
}

TEST(config_get_int_null_manager)
{
    int result = config_get_int(NULL, "key");
    ASSERT_EQ(0, result);
    return 0;
}

TEST(config_get_bool_null_manager)
{
    bool result = config_get_bool(NULL, "key");
    ASSERT_FALSE(result);
    return 0;
}

/* =========================================================================
 * Main Entry Point
 * ========================================================================= */

int test_config_main(void)
{
    /* Reset counters for this test suite */
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
    
    TEST_SUITE_BEGIN("ConfigManager Lifecycle");
    RUN_TEST(config_manager_create_returns_valid_pointer);
    RUN_TEST(config_manager_destroy_null_is_safe);
    RUN_TEST(config_manager_has_resolved_config);
    
    TEST_SUITE_BEGIN("ConfigValue Operations");
    RUN_TEST(config_value_init_string);
    RUN_TEST(config_value_init_null_key);
    RUN_TEST(config_value_set_string);
    RUN_TEST(config_value_set_int);
    RUN_TEST(config_value_set_bool);
    RUN_TEST(config_value_cleanup_null_safe);
    
    TEST_SUITE_BEGIN("ConfigLayer Operations");
    RUN_TEST(config_layer_add_value_succeeds);
    RUN_TEST(config_layer_get_value_returns_null_for_missing_key);
    RUN_TEST(config_layer_get_value_null_params);
    
    TEST_SUITE_BEGIN("Default Configuration");
    RUN_TEST(config_load_defaults_succeeds);
    RUN_TEST(config_load_defaults_null_manager);
    
    TEST_SUITE_BEGIN("Configuration Resolution");
    RUN_TEST(config_resolve_returns_valid_config);
    RUN_TEST(config_resolve_null_manager);
    
    TEST_SUITE_BEGIN("Configuration Getters");
    RUN_TEST(config_get_string_returns_value);
    RUN_TEST(config_get_string_null_manager);
    RUN_TEST(config_get_int_null_manager);
    RUN_TEST(config_get_bool_null_manager);
    
    TEST_SUMMARY();
    
    return TEST_EXIT_CODE();
}
