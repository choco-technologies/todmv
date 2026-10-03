#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libtodmv.h"

static libtodmv_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = libtodmv_create();
}

void dmod_test_teardown(void)
{
    libtodmv_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(libtodmv_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(libtodmv_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(libtodmv_is_valid(g_handle));
}

DMOD_TEST_STEP(libtodmv_destroy_null)
{
    /* Destroying NULL must not crash. */
    libtodmv_destroy(NULL);
}
