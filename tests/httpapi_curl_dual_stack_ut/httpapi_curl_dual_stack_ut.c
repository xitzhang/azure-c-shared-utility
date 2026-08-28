// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "curl/curl.h"

static CURLcode test_curl_global_init(long flags);
static curl_version_info_data* test_curl_version_info(CURLversion age);
static void test_curl_global_cleanup(void);
static CURL* test_curl_easy_init(void);
static CURLcode test_curl_easy_setopt(CURL* curl, CURLoption option, ...);
static const char* test_curl_easy_strerror(CURLcode error);
static void test_curl_easy_cleanup(CURL* curl);
static void* test_malloc(size_t size);
static void test_free(void* pointer);

#define HTTPAPI_CURL_GLOBAL_INIT test_curl_global_init
#define HTTPAPI_CURL_VERSION_INFO test_curl_version_info
#define HTTPAPI_CURL_GLOBAL_CLEANUP test_curl_global_cleanup
#define HTTPAPI_CURL_EASY_INIT test_curl_easy_init
#define HTTPAPI_CURL_EASY_SETOPT test_curl_easy_setopt
#define HTTPAPI_CURL_EASY_STRERROR test_curl_easy_strerror
#define HTTPAPI_CURL_EASY_CLEANUP test_curl_easy_cleanup
#define HTTPAPI_CURL_MALLOC test_malloc
#define HTTPAPI_CURL_FREE test_free
#include "../../adapters/httpapi_curl.c"

#define TEST_CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            (void)fprintf(stderr, "Test failure at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return __LINE__; \
        } \
    } while (0)

static curl_version_info_data mock_version_info;
static curl_version_info_data* mock_version_info_result;
static CURLcode mock_global_init_result;
static CURLcode mock_setopt_result;
static int global_init_count;
static int version_info_count;
static int global_cleanup_count;
static int easy_init_count;
static int easy_setopt_count;
static int easy_cleanup_count;
static int allocation_count;
static int ipresolve_setopt_count;
static CURLoption last_setopt_option;
static long happy_eyeballs_timeout_ms;

static void reset_mocks(void)
{
    (void)memset(&mock_version_info, 0, sizeof(mock_version_info));
    mock_version_info.version_num = 0x073B00;
    mock_version_info.features = CURL_VERSION_IPV6;
    mock_version_info_result = &mock_version_info;
    mock_global_init_result = CURLE_OK;
    mock_setopt_result = CURLE_OK;
    global_init_count = 0;
    version_info_count = 0;
    global_cleanup_count = 0;
    easy_init_count = 0;
    easy_setopt_count = 0;
    easy_cleanup_count = 0;
    allocation_count = 0;
    ipresolve_setopt_count = 0;
    last_setopt_option = (CURLoption)0;
    happy_eyeballs_timeout_ms = -1;
    nUsersOfHTTPAPI = 0;
}

static CURLcode test_curl_global_init(long flags)
{
    (void)flags;
    global_init_count++;
    return mock_global_init_result;
}

static curl_version_info_data* test_curl_version_info(CURLversion age)
{
    (void)age;
    version_info_count++;
    return mock_version_info_result;
}

static void test_curl_global_cleanup(void)
{
    global_cleanup_count++;
}

static CURL* test_curl_easy_init(void)
{
    easy_init_count++;
    return (CURL*)&mock_version_info;
}

static CURLcode test_curl_easy_setopt(CURL* curl, CURLoption option, ...)
{
    va_list arguments;
    (void)curl;
    easy_setopt_count++;
    last_setopt_option = option;
    if (option == CURLOPT_IPRESOLVE)
    {
        ipresolve_setopt_count++;
    }
    if (option == CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS)
    {
        va_start(arguments, option);
        happy_eyeballs_timeout_ms = va_arg(arguments, long);
        va_end(arguments);
    }
    return mock_setopt_result;
}

static const char* test_curl_easy_strerror(CURLcode error)
{
    (void)error;
    return "mock curl error";
}

static void test_curl_easy_cleanup(CURL* curl)
{
    (void)curl;
    easy_cleanup_count++;
}

static void* test_malloc(size_t size)
{
    void* result = malloc(size);
    if (result != NULL)
    {
        allocation_count++;
    }
    return result;
}

static void test_free(void* pointer)
{
    if (pointer != NULL)
    {
        allocation_count--;
    }
    free(pointer);
}

static int init_rejects_null_version_info(void)
{
    reset_mocks();
    mock_version_info_result = NULL;

    TEST_CHECK(HTTPAPI_Init() == HTTPAPI_INIT_FAILED);
    TEST_CHECK(global_init_count == 1);
    TEST_CHECK(version_info_count == 1);
    TEST_CHECK(global_cleanup_count == 1);
    TEST_CHECK(nUsersOfHTTPAPI == 0);
    return 0;
}

static int init_rejects_old_runtime(void)
{
    reset_mocks();
    mock_version_info.version_num = 0x073A00;

    TEST_CHECK(HTTPAPI_Init() == HTTPAPI_INIT_FAILED);
    TEST_CHECK(global_cleanup_count == 1);
    TEST_CHECK(nUsersOfHTTPAPI == 0);
    return 0;
}

static int init_rejects_runtime_without_ipv6(void)
{
    reset_mocks();
    mock_version_info.features = 0;

    TEST_CHECK(HTTPAPI_Init() == HTTPAPI_INIT_FAILED);
    TEST_CHECK(global_cleanup_count == 1);
    TEST_CHECK(nUsersOfHTTPAPI == 0);
    return 0;
}

static int init_reference_counting_validates_only_first_user(void)
{
    reset_mocks();

    TEST_CHECK(HTTPAPI_Init() == HTTPAPI_OK);
    TEST_CHECK(HTTPAPI_Init() == HTTPAPI_OK);
    TEST_CHECK(global_init_count == 1);
    TEST_CHECK(version_info_count == 1);
    TEST_CHECK(nUsersOfHTTPAPI == 2);

    HTTPAPI_Deinit();
    TEST_CHECK(global_cleanup_count == 0);
    TEST_CHECK(nUsersOfHTTPAPI == 1);
    TEST_CHECK(HTTPAPI_Init() == HTTPAPI_OK);
    TEST_CHECK(global_init_count == 1);
    TEST_CHECK(version_info_count == 1);

    HTTPAPI_Deinit();
    HTTPAPI_Deinit();
    TEST_CHECK(global_cleanup_count == 1);
    TEST_CHECK(nUsersOfHTTPAPI == 0);
    return 0;
}

static int create_sets_happy_eyeballs_timeout(void)
{
    HTTP_HANDLE handle;

    reset_mocks();
    handle = HTTPAPI_CreateConnection("example.test");
    TEST_CHECK(handle != NULL);
    TEST_CHECK(easy_init_count == 1);
    TEST_CHECK(easy_setopt_count == 1);
    TEST_CHECK(last_setopt_option == CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS);
    TEST_CHECK(happy_eyeballs_timeout_ms == 250L);
    TEST_CHECK(ipresolve_setopt_count == 0);
    TEST_CHECK(allocation_count == 2);

    HTTPAPI_CloseConnection(handle);
    TEST_CHECK(easy_cleanup_count == 1);
    TEST_CHECK(allocation_count == 0);
    return 0;
}

static int create_setopt_failure_cleans_everything(void)
{
    reset_mocks();
    mock_setopt_result = CURLE_UNKNOWN_OPTION;

    TEST_CHECK(HTTPAPI_CreateConnection("example.test") == NULL);
    TEST_CHECK(easy_init_count == 1);
    TEST_CHECK(easy_setopt_count == 1);
    TEST_CHECK(ipresolve_setopt_count == 0);
    TEST_CHECK(easy_cleanup_count == 1);
    TEST_CHECK(allocation_count == 0);
    return 0;
}

int main(void)
{
    int result;

    result = init_rejects_null_version_info();
    if (result == 0) result = init_rejects_old_runtime();
    if (result == 0) result = init_rejects_runtime_without_ipv6();
    if (result == 0) result = init_reference_counting_validates_only_first_user();
    if (result == 0) result = create_sets_happy_eyeballs_timeout();
    if (result == 0) result = create_setopt_failure_cleans_everything();

    if (result == 0)
    {
        (void)printf("httpapi_curl_dual_stack_ut passed\n");
    }
    return result;
}
