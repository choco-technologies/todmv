#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "todmv.h"

static todmv_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = todmv_create();
}

void dmod_test_teardown(void)
{
    todmv_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(todmv_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(todmv_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(todmv_is_valid(g_handle));
}

DMOD_TEST_STEP(todmv_destroy_null)
{
    /* Destroying NULL must not crash. */
    todmv_destroy(NULL);
}
