// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

// Unit tests for the Berkeley socket adapter's connect loop.
//
// This suite previously contained only #if 0 blocks written against MicroMock,
// which was retired: between BEGIN_TEST_SUITE and END_TEST_SUITE every test was
// disabled, so the target built and ran zero tests. The cases below are the
// umock_c replacement, and they cover the behaviour socketio_win32_ut already
// covers on the Windows side so the two adapters are held to the same contract.
//
// What is asserted is the candidate loop: how many attempts are made, in which
// address family, how much time each one is granted, and whether a failing
// candidate stops the open. Those are the properties the per-address timeout
// exists to provide.
//
// The tests assert recorded behaviour rather than a strict umock call trace.
// The connect path makes several calls whose order is an implementation detail,
// and pinning the full trace would make the suite fail on harmless refactors
// instead of on a behaviour change.

#ifdef __cplusplus
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#else
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#endif

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "testrunnerswitcher.h"
#include "umock_c.h"
#include "umocktypes_charptr.h"

static void* my_gballoc_malloc(size_t size)
{
    return malloc(size);
}

// gballoc.h redirects calloc/realloc as well as malloc once ENABLE_MOCKS is on.
// Without hooks these return NULL by default, which silently turns every
// allocation in this file into a null dereference.
static void* my_gballoc_calloc(size_t nmemb, size_t size)
{
    return calloc(nmemb, size);
}

static void* my_gballoc_realloc(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

static void my_gballoc_free(void* ptr)
{
    free(ptr);
}

#define ENABLE_MOCKS
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/optionhandler.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#undef ENABLE_MOCKS

#include "azure_c_shared_utility/socketio.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/xio.h"

// The adapter's own per-address grant. Kept as a literal on purpose: if the
// product constant changes, these tests should fail and be re-read rather than
// silently follow it.
#define EXPECTED_PER_ADDRESS_TIMEOUT_MS 10000

// The adapter's stand-in for ETIMEDOUT when poll() runs out the grant. Mirrors
// SOCKETIO_POLL_TIMEOUT_ERROR in socketio_berkeley.c, which is file-local.
#define SOCKETIO_POLL_TIMEOUT_ERROR_CODE 110

#define PORT_NUM 80
#define HOSTNAME_ARG "hostname"

#define MAX_CANDIDATES 8

// How the mocked connect/poll pair should behave for each attempt, in order.
typedef enum ATTEMPT_OUTCOME_TAG
{
    ATTEMPT_TIMES_OUT,
    ATTEMPT_SUCCEEDS,
    ATTEMPT_REFUSED,
    // poll() is interrupted by a signal once (-1, EINTR) and then reports the
    // socket as settled; SO_ERROR reads 0.
    ATTEMPT_EINTR_THEN_SUCCEEDS,
    // poll() itself fails with EBADF (not a timeout, not EINTR).
    ATTEMPT_POLL_FAILS
} ATTEMPT_OUTCOME;

// Candidate "families" that make the mocked getaddrinfo hand back a malformed
// entry, so the adapter's pre-socket validation can be exercised.
#define CANDIDATE_INVALID_FAMILY (-1)   /* ai_family is AF_UNIX */
#define CANDIDATE_SHORT_ADDRLEN (-2)    /* AF_INET6 with a sockaddr_in-sized length */

static int g_candidate_families[MAX_CANDIDATES];
static size_t g_candidate_count;
static ATTEMPT_OUTCOME g_attempt_outcomes[MAX_CANDIDATES];

// Observed during the run.
static int g_connect_families[MAX_CANDIDATES];
static size_t g_connect_attempt_count;
static int g_poll_timeouts_ms[MAX_CANDIDATES];
static size_t g_poll_count;
static size_t g_poll_count_at_attempt_start;
static int g_socket_domains[MAX_CANDIDATES];
static int g_socket_types[MAX_CANDIDATES];
static size_t g_socket_count;
static size_t g_v6only_cleared_count;
static int g_v6only_last_value;
static int g_last_addrinfo_family;
static int g_last_addrinfo_flags;
static int g_retrieved_enable_ipv6;
static pfCloneOption g_retrieved_clone_option;
static pfDestroyOption g_retrieved_destroy_option;

static IO_OPEN_RESULT_DETAILED g_open_result;
static size_t g_open_complete_count;
static int g_getaddrinfo_result;

// Number of descriptors the process currently holds. Comparing this around an
// open is a direct check that failed candidates released their sockets - a
// stronger statement than counting calls to a mocked close().
static size_t open_fd_count(void)
{
    size_t count = 0;
    DIR* dir = opendir("/proc/self/fd");
    if (dir != NULL)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_name[0] != '.')
            {
                count++;
            }
        }
        (void)closedir(dir);
    }
    return count;
}

static const SINGLYLINKEDLIST_HANDLE TEST_SINGLYLINKEDLIST_HANDLE = (SINGLYLINKEDLIST_HANDLE)0x4242;
static const void** list_items = NULL;
static size_t list_item_count = 0;
static bool singlylinkedlist_add_called = false;

static TEST_MUTEX_HANDLE g_testByTest;
static TEST_MUTEX_HANDLE g_dllByDll;

static ATTEMPT_OUTCOME current_attempt_outcome(void)
{
    ATTEMPT_OUTCOME result = ATTEMPT_TIMES_OUT;

    if ((g_connect_attempt_count > 0) && (g_connect_attempt_count <= MAX_CANDIDATES))
    {
        result = g_attempt_outcomes[g_connect_attempt_count - 1];
    }

    return result;
}

// Hands out a fresh real descriptor per attempt. It must be real because the
// adapter calls fcntl() on it, and fcntl is variadic so umock_c cannot mock it.
// A fresh one per call means the adapter's own close() genuinely releases it,
// which is what lets the leak check below mean something.
MOCK_FUNCTION_WITH_CODE(, int, socket, int, domain, int, type, int, protocol)
int new_fd = open("/dev/null", O_RDWR);
if (g_socket_count < MAX_CANDIDATES)
{
    g_socket_domains[g_socket_count] = domain;
    g_socket_types[g_socket_count] = type;
}
g_socket_count++;
MOCK_FUNCTION_END(new_fd)

// close() is deliberately NOT mocked. It is a libc symbol the whole process
// shares - the test runtime and stdio call it too - so interposing it here
// would route unrelated calls into umock_c, including calls made before
// umock_c_init. Descriptor hygiene is checked directly instead, by counting
// /proc/self/fd around the open.

MOCK_FUNCTION_WITH_CODE(, int, connect, int, sockfd, const struct sockaddr*, addr, socklen_t, addrlen)
if ((addr != NULL) && (g_connect_attempt_count < MAX_CANDIDATES))
{
    g_connect_families[g_connect_attempt_count] = addr->sa_family;
}
g_connect_attempt_count++;
g_poll_count_at_attempt_start = g_poll_count;
// Always report "in progress" so the adapter takes the poll path, which is
// where the per-address grant is applied.
errno = EINPROGRESS;
MOCK_FUNCTION_END(-1)

MOCK_FUNCTION_WITH_CODE(, int, poll, struct pollfd*, fds, nfds_t, nfds, int, timeout)
int poll_result;
ATTEMPT_OUTCOME outcome = current_attempt_outcome();
if (g_poll_count < MAX_CANDIDATES)
{
    g_poll_timeouts_ms[g_poll_count] = timeout;
}
g_poll_count++;
if ((outcome == ATTEMPT_EINTR_THEN_SUCCEEDS) && (g_poll_count - g_poll_count_at_attempt_start == 1))
{
    // First poll of this attempt is interrupted by a signal.
    errno = EINTR;
    poll_result = -1;
}
else if (outcome == ATTEMPT_POLL_FAILS)
{
    errno = EBADF;
    poll_result = -1;
}
else
{
    // 0 means the grant ran out; anything positive means the socket settled and
    // the adapter goes on to read SO_ERROR.
    poll_result = (outcome == ATTEMPT_TIMES_OUT) ? 0 : 1;
}
MOCK_FUNCTION_END(poll_result)

MOCK_FUNCTION_WITH_CODE(, int, getsockopt, int, sockfd, int, level, int, optname, void*, optval, socklen_t*, optlen)
if (optval != NULL)
{
    *(int*)optval = (current_attempt_outcome() == ATTEMPT_REFUSED) ? ECONNREFUSED : 0;
}
MOCK_FUNCTION_END(0)

MOCK_FUNCTION_WITH_CODE(, int, setsockopt, int, sockfd, int, level, int, optname, const void*, optval, socklen_t, optlen)
if ((level == IPPROTO_IPV6) && (optname == IPV6_V6ONLY) && (optval != NULL))
{
    g_v6only_cleared_count++;
    g_v6only_last_value = *(const int*)optval;
}
MOCK_FUNCTION_END(0)

MOCK_FUNCTION_WITH_CODE(, int, getaddrinfo, const char*, node, const char*, service, const struct addrinfo*, hints, struct addrinfo**, res)
size_t candidate_index;
struct addrinfo* head = NULL;
struct addrinfo* tail = NULL;
int getaddrinfo_result = g_getaddrinfo_result;
g_last_addrinfo_family = (hints != NULL) ? hints->ai_family : -1;
g_last_addrinfo_flags = (hints != NULL) ? hints->ai_flags : -1;
for (candidate_index = 0; (getaddrinfo_result == 0) && (candidate_index < g_candidate_count); candidate_index++)
{
    struct addrinfo* entry = (struct addrinfo*)calloc(1, sizeof(struct addrinfo));
    int family = g_candidate_families[candidate_index];
    entry->ai_family = (family == CANDIDATE_INVALID_FAMILY) ? AF_UNIX : ((family == CANDIDATE_SHORT_ADDRLEN) ? AF_INET6 : family);
    entry->ai_socktype = SOCK_STREAM;
    entry->ai_protocol = IPPROTO_TCP;
    if (family == CANDIDATE_SHORT_ADDRLEN)
    {
        // An AF_INET6 entry whose address is only sockaddr_in-sized.
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)calloc(1, sizeof(struct sockaddr_in6));
        sa6->sin6_family = AF_INET6;
        entry->ai_addr = (struct sockaddr*)sa6;
        entry->ai_addrlen = sizeof(struct sockaddr_in);
    }
    else if (family == AF_INET6)
    {
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)calloc(1, sizeof(struct sockaddr_in6));
        sa6->sin6_family = AF_INET6;
        entry->ai_addr = (struct sockaddr*)sa6;
        entry->ai_addrlen = sizeof(struct sockaddr_in6);
    }
    else
    {
        struct sockaddr_in* sa4 = (struct sockaddr_in*)calloc(1, sizeof(struct sockaddr_in));
        sa4->sin_family = AF_INET;
        entry->ai_addr = (struct sockaddr*)sa4;
        entry->ai_addrlen = sizeof(struct sockaddr_in);
    }
    if (head == NULL)
    {
        head = entry;
    }
    else
    {
        tail->ai_next = entry;
    }
    tail = entry;
}
*res = head;
MOCK_FUNCTION_END(getaddrinfo_result)

MOCK_FUNCTION_WITH_CODE(, void, freeaddrinfo, struct addrinfo*, res)
while (res != NULL)
{
    struct addrinfo* next = res->ai_next;
    free(res->ai_addr);
    free(res);
    res = next;
}
MOCK_FUNCTION_END()

static LIST_ITEM_HANDLE my_singlylinkedlist_get_head_item(SINGLYLINKEDLIST_HANDLE list)
{
    LIST_ITEM_HANDLE listHandle = NULL;
    (void)list;
    if (list_item_count > 0)
    {
        listHandle = (LIST_ITEM_HANDLE)list_items[0];
        list_item_count--;
    }
    return listHandle;
}

static LIST_ITEM_HANDLE my_singlylinkedlist_add(SINGLYLINKEDLIST_HANDLE list, const void* item)
{
    const void** items = (const void**)realloc((void*)list_items, (list_item_count + 1) * sizeof(item));
    (void)list;
    if (items != NULL)
    {
        list_items = items;
        list_items[list_item_count++] = item;
    }
    singlylinkedlist_add_called = true;
    return (LIST_ITEM_HANDLE)list_item_count;
}

static const void* my_singlylinkedlist_item_get_value(LIST_ITEM_HANDLE item_handle)
{
    return singlylinkedlist_add_called ? (const void*)item_handle : NULL;
}

static LIST_ITEM_HANDLE my_singlylinkedlist_find(SINGLYLINKEDLIST_HANDLE handle, LIST_MATCH_FUNCTION match_function, const void* match_context)
{
    size_t i;
    const void* found_item = NULL;
    (void)handle;
    for (i = 0; i < list_item_count; i++)
    {
        if (match_function((LIST_ITEM_HANDLE)list_items[i], match_context))
        {
            found_item = list_items[i];
            break;
        }
    }
    return (LIST_ITEM_HANDLE)found_item;
}

static void my_singlylinkedlist_destroy(SINGLYLINKEDLIST_HANDLE handle)
{
    (void)handle;
    free((void*)list_items);
    list_items = NULL;
    list_item_count = 0;
}

static void test_on_bytes_received(void* context, const unsigned char* buffer, size_t size)
{
    (void)context;
    (void)buffer;
    (void)size;
}

static void test_on_io_open_complete(void* context, IO_OPEN_RESULT_DETAILED open_result)
{
    (void)context;
    g_open_complete_count++;
    g_open_result = open_result;
}

static void test_on_io_error(void* context)
{
    (void)context;
}

static OPTIONHANDLER_HANDLE test_OptionHandler_Create(pfCloneOption cloneOption, pfDestroyOption destroyOption, pfSetOption setOption)
{
    (void)setOption;
    g_retrieved_clone_option = cloneOption;
    g_retrieved_destroy_option = destroyOption;
    return (OPTIONHANDLER_HANDLE)0x4243;
}

static OPTIONHANDLER_RESULT test_OptionHandler_AddOption(OPTIONHANDLER_HANDLE handle, const char* name, const void* value)
{
    (void)handle;
    if ((name != NULL) && (value != NULL) && (strcmp(name, OPTION_ENABLE_IPV6) == 0))
    {
        g_retrieved_enable_ipv6 = *(const int*)value;
    }
    return OPTIONHANDLER_OK;
}

static void on_umock_c_error(UMOCK_C_ERROR_CODE error_code)
{
    char temp_str[256];
    (void)snprintf(temp_str, sizeof(temp_str), "umock_c reported error :%d", (int)error_code);
    ASSERT_FAIL(temp_str);
}

// Declares the addresses getaddrinfo should return, in order, and how each
// attempt against them should behave.
static void given_candidates(size_t count, const int* families, const ATTEMPT_OUTCOME* outcomes)
{
    size_t i;
    g_candidate_count = count;
    for (i = 0; i < count; i++)
    {
        g_candidate_families[i] = families[i];
        g_attempt_outcomes[i] = outcomes[i];
    }
}

static CONCRETE_IO_HANDLE create_socket_io(const char* hostname, int enable_ipv6)
{
    SOCKETIO_CONFIG socketConfig = { hostname, PORT_NUM, NULL, enable_ipv6 };
    CONCRETE_IO_HANDLE ioHandle = socketio_create(&socketConfig);
    ASSERT_IS_NOT_NULL(ioHandle);
    return ioHandle;
}

BEGIN_TEST_SUITE(socketio_berkeley_unittests)

TEST_SUITE_INITIALIZE(suite_init)
{
    int result;

    TEST_INITIALIZE_MEMORY_DEBUG(g_dllByDll);
    g_testByTest = TEST_MUTEX_CREATE();
    ASSERT_IS_NOT_NULL(g_testByTest);

    umock_c_init(on_umock_c_error);

    result = umocktypes_charptr_register_types();
    ASSERT_ARE_EQUAL(int, 0, result);

    REGISTER_UMOCK_ALIAS_TYPE(CONCRETE_IO_HANDLE, void*);
    REGISTER_UMOCK_ALIAS_TYPE(SINGLYLINKEDLIST_HANDLE, void*);
    REGISTER_UMOCK_ALIAS_TYPE(LIST_ITEM_HANDLE, void*);
    REGISTER_UMOCK_ALIAS_TYPE(LIST_MATCH_FUNCTION, void*);
    REGISTER_UMOCK_ALIAS_TYPE(LIST_ACTION_FUNCTION, void*);
    REGISTER_UMOCK_ALIAS_TYPE(LIST_CONDITION_FUNCTION, void*);
    REGISTER_UMOCK_ALIAS_TYPE(OPTIONHANDLER_HANDLE, void*);
    REGISTER_UMOCK_ALIAS_TYPE(pfCloneOption, void*);
    REGISTER_UMOCK_ALIAS_TYPE(pfDestroyOption, void*);
    REGISTER_UMOCK_ALIAS_TYPE(pfSetOption, void*);
    REGISTER_UMOCK_ALIAS_TYPE(socklen_t, unsigned int);
    REGISTER_UMOCK_ALIAS_TYPE(nfds_t, unsigned long);
    // Every pointer argument that appears in a mock needs a registered type, or
    // umock_c has no handler to copy/stringify it with when it records the call.
    REGISTER_UMOCK_ALIAS_TYPE(socklen_t*, void*);
    REGISTER_UMOCK_ALIAS_TYPE(const void*, void*);
    REGISTER_UMOCK_ALIAS_TYPE(struct pollfd*, void*);
    REGISTER_UMOCK_ALIAS_TYPE(const struct sockaddr*, void*);
    REGISTER_UMOCK_ALIAS_TYPE(struct addrinfo*, void*);
    REGISTER_UMOCK_ALIAS_TYPE(struct addrinfo**, void*);
    REGISTER_UMOCK_ALIAS_TYPE(const struct addrinfo*, void*);
    REGISTER_UMOCK_ALIAS_TYPE(const char*, char*);

    REGISTER_GLOBAL_MOCK_RETURN(singlylinkedlist_create, TEST_SINGLYLINKEDLIST_HANDLE);
    REGISTER_GLOBAL_MOCK_RETURN(singlylinkedlist_remove, 0);
    REGISTER_GLOBAL_MOCK_HOOK(gballoc_malloc, my_gballoc_malloc);
    REGISTER_GLOBAL_MOCK_HOOK(gballoc_calloc, my_gballoc_calloc);
    REGISTER_GLOBAL_MOCK_HOOK(gballoc_realloc, my_gballoc_realloc);
    REGISTER_GLOBAL_MOCK_HOOK(gballoc_free, my_gballoc_free);
    REGISTER_GLOBAL_MOCK_HOOK(singlylinkedlist_get_head_item, my_singlylinkedlist_get_head_item);
    REGISTER_GLOBAL_MOCK_HOOK(singlylinkedlist_add, my_singlylinkedlist_add);
    REGISTER_GLOBAL_MOCK_HOOK(singlylinkedlist_item_get_value, my_singlylinkedlist_item_get_value);
    REGISTER_GLOBAL_MOCK_HOOK(singlylinkedlist_find, my_singlylinkedlist_find);
    REGISTER_GLOBAL_MOCK_HOOK(singlylinkedlist_destroy, my_singlylinkedlist_destroy);
    REGISTER_GLOBAL_MOCK_HOOK(OptionHandler_Create, test_OptionHandler_Create);
    REGISTER_GLOBAL_MOCK_HOOK(OptionHandler_AddOption, test_OptionHandler_AddOption);
}

TEST_SUITE_CLEANUP(suite_cleanup)
{
    umock_c_deinit();

    TEST_MUTEX_DESTROY(g_testByTest);
    TEST_DEINITIALIZE_MEMORY_DEBUG(g_dllByDll);
}

TEST_FUNCTION_INITIALIZE(method_init)
{
    if (TEST_MUTEX_ACQUIRE(g_testByTest))
    {
        ASSERT_FAIL("Could not acquire test serialization mutex.");
    }

    umock_c_reset_all_calls();

    memset(g_candidate_families, 0, sizeof(g_candidate_families));
    memset(g_attempt_outcomes, 0, sizeof(g_attempt_outcomes));
    memset(g_connect_families, 0, sizeof(g_connect_families));
    memset(g_poll_timeouts_ms, 0, sizeof(g_poll_timeouts_ms));
    memset(g_socket_domains, 0, sizeof(g_socket_domains));
    memset(g_socket_types, 0, sizeof(g_socket_types));
    g_candidate_count = 0;
    g_connect_attempt_count = 0;
    g_poll_count = 0;
    g_poll_count_at_attempt_start = 0;
    g_socket_count = 0;
    g_v6only_cleared_count = 0;
    g_v6only_last_value = -1;
    g_last_addrinfo_family = -1;
    g_last_addrinfo_flags = -1;
    g_retrieved_enable_ipv6 = -1;
    g_retrieved_clone_option = NULL;
    g_retrieved_destroy_option = NULL;
    list_item_count = 0;
    singlylinkedlist_add_called = false;
    g_open_result.result = IO_OPEN_CANCELLED;
    g_open_result.code = 0;
    g_open_complete_count = 0;
    g_getaddrinfo_result = 0;
}

TEST_FUNCTION_CLEANUP(method_cleanup)
{
    TEST_MUTEX_RELEASE(g_testByTest);
}

/* Every resolved address is granted the whole per-address timeout. There is no
   budget divided between candidates, so the second attempt must be offered just
   as much time as the first. */
TEST_FUNCTION(socketio_open_grants_every_address_the_full_timeout)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_TIMES_OUT, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int result;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, 0, result);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[0]);
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[1]);

    socketio_destroy(ioHandle);
}

/* However many black-holed addresses of one family come first, the family
   behind them still has to get its attempt. Each address has its own grant. */
TEST_FUNCTION(socketio_open_reaches_ipv4_after_two_blackholed_ipv6_candidates)
{
    const int families[] = { AF_INET6, AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_TIMES_OUT, ATTEMPT_TIMES_OUT, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int result;

    given_candidates(3, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, 0, result);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)3, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[0]);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[1]);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[2]);
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[2]);

    socketio_destroy(ioHandle);
}

/* Three candidates exercise repeated per-address grants and ensure the later
   IPv4 candidate is not starved by earlier timeouts. */
TEST_FUNCTION(socketio_open_reaches_ipv4_after_three_blackholed_ipv6_candidates)
{
    const int families[] = { AF_INET6, AF_INET6, AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_TIMES_OUT, ATTEMPT_TIMES_OUT, ATTEMPT_TIMES_OUT, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int result;

    given_candidates(4, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, 0, result);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)4, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[3]);
    // The last candidate is granted the same amount as the first: nothing was
    // deducted from it by the three attempts in front of it.
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[0]);
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[3]);

    socketio_destroy(ioHandle);
}

/* A refused address returns immediately and must not cost the addresses behind
   it anything - neither their attempt nor any part of their grant. */
TEST_FUNCTION(socketio_open_refused_address_does_not_reduce_the_next_grant)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_REFUSED, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int result;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, 0, result);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[1]);

    socketio_destroy(ioHandle);
}

/* When every candidate fails the open has to fail too and leave no socket
   behind. */
TEST_FUNCTION(socketio_open_fails_and_releases_every_socket_when_all_candidates_time_out)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_TIMES_OUT, ATTEMPT_TIMES_OUT };
    CONCRETE_IO_HANDLE ioHandle;
    int result;
    size_t fds_before;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    fds_before = open_fd_count();

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    // socketio_open returns 0 once it has invoked the callback - the outcome is
    // carried in the callback so upstream layers can surface the error code -
    // so the failure has to be read from the reported result, not the return.
    ASSERT_ARE_EQUAL(int, 0, result);
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    ASSERT_ARE_EQUAL(int, SOCKETIO_POLL_TIMEOUT_ERROR_CODE, g_open_result.code);
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_connect_attempt_count);
    // Two sockets were created and both attempts failed, so the process must
    // hold no more descriptors than it did before the open.
    ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());

    socketio_destroy(ioHandle);
}

#ifndef __APPLE__
TEST_FUNCTION(socketio_open_preserves_network_interface_enumeration_error)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);
    ASSERT_ARE_EQUAL(int, 0, socketio_setoption(ioHandle, OPTION_NET_INT_MAC_ADDRESS, "00:11:22:33:44:55"));

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    ASSERT_ARE_EQUAL(int, ENOTTY, g_open_result.code);

    socketio_destroy(ioHandle);
}
#endif

/* IPv4-mapped destinations such as ::ffff:203.0.113.1 resolve to AF_INET6 and
   can only be delivered by a dual-stack socket, so IPV6_V6ONLY has to be
   cleared - and only on AF_INET6 sockets, where the option means anything. */
TEST_FUNCTION(socketio_open_clears_ipv6_v6only_on_inet6_sockets_only)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_TIMES_OUT, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int result;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, 0, result);
    // Two candidates were tried but only one of them was AF_INET6.
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_v6only_cleared_count);
    ASSERT_ARE_EQUAL(int, 0, g_v6only_last_value);

    socketio_destroy(ioHandle);
}

/* A single resolved address is the common case and must still get the full
   grant - the loop must not treat "last candidate" as a special, smaller one. */
TEST_FUNCTION(socketio_open_single_address_gets_the_full_timeout)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_TIMES_OUT };
    CONCRETE_IO_HANDLE ioHandle;
    int result;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, 0, result);
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[0]);

    socketio_destroy(ioHandle);
}

/* The first candidate working must end the loop: no further address is tried. */
TEST_FUNCTION(socketio_open_stops_at_the_first_candidate_that_connects)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int result;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    result = socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, 0, result);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_without_ipv6_opt_in_requests_ipv4_only)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_INET, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, 0, g_last_addrinfo_flags);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_retrieveoptions_preserves_ipv6_opt_in)
{
    CONCRETE_IO_HANDLE ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    OPTIONHANDLER_HANDLE options = socketio_get_interface_description()->concrete_io_retrieveoptions(ioHandle);
    int* cloned_value;

    ASSERT_ARE_EQUAL(void_ptr, (OPTIONHANDLER_HANDLE)0x4243, options);
    ASSERT_ARE_EQUAL(int, 1, g_retrieved_enable_ipv6);
    ASSERT_IS_NOT_NULL(g_retrieved_clone_option);
    ASSERT_IS_NOT_NULL(g_retrieved_destroy_option);

    cloned_value = (int*)g_retrieved_clone_option(OPTION_ENABLE_IPV6, &g_retrieved_enable_ipv6);
    ASSERT_IS_NOT_NULL(cloned_value);
    ASSERT_ARE_EQUAL(int, 1, *cloned_value);
    g_retrieved_destroy_option(OPTION_ENABLE_IPV6, cloned_value);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_with_ipv6_opt_in_requests_both_families)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_UNSPEC, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, 0, g_last_addrinfo_flags);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_ipv6_literal_without_opt_in_requests_ipv4_only)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io("::1", 0);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_INET, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_with_ipv6_opt_in_preserves_ipv4_fallback)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_REFUSED, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_UNSPEC, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[1]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_setoption_enable_ipv6_before_open_enables_dual_stack_resolution)
{
    const int families[] = { AF_INET6 };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int enable_ipv6 = 1;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 0);

    ASSERT_ARE_EQUAL(int, 0, socketio_setoption(ioHandle, OPTION_ENABLE_IPV6, &enable_ipv6));
    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_UNSPEC, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

/* Literals follow the opt-in like hostnames do: without it the lookup is the
   pre-IPv6 AF_INET one, with it both families are requested. */
TEST_FUNCTION(socketio_open_ipv4_literal_without_opt_in_requests_ipv4_only)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io("127.0.0.1", 0);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_INET, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_ipv4_literal_with_opt_in_requests_both_families)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io("127.0.0.1", 1);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_UNSPEC, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_ipv6_literal_with_opt_in_requests_both_families)
{
    const int families[] = { AF_INET6 };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io("::1", 1);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_UNSPEC, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[0]);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_ipv4_mapped_literal_without_opt_in_requests_ipv4_only)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io("::ffff:127.0.0.1", 0);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_INET, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)0, g_v6only_cleared_count);

    socketio_destroy(ioHandle);
}

/* With the opt-in, a mapped literal resolves to an AF_INET6 candidate, which only
   reaches its IPv4 destination from a dual-stack socket. */
TEST_FUNCTION(socketio_open_ipv4_mapped_literal_with_opt_in_uses_a_dual_stack_socket)
{
    const int families[] = { AF_INET6 };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io("::ffff:127.0.0.1", 1);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_UNSPEC, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[0]);
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_v6only_cleared_count);
    ASSERT_ARE_EQUAL(int, 0, g_v6only_last_value);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_setoption_can_turn_the_ipv6_opt_in_back_off_before_open)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    int enable_ipv6 = 0;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);

    ASSERT_ARE_EQUAL(int, 0, socketio_setoption(ioHandle, OPTION_ENABLE_IPV6, &enable_ipv6));
    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, AF_INET, g_last_addrinfo_family);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);

    socketio_destroy(ioHandle);
}

/* The opt-in belongs to the IO instance, not to one connection: an IO that is
   closed and opened again, as a WebSocket client does when it reconnects, keeps
   asking the resolver for both families every time and releases each socket. */
TEST_FUNCTION(socketio_open_keeps_the_ipv6_opt_in_across_close_and_reopen)
{
    const int families[] = { AF_INET6 };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;
    int cycle;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    for (cycle = 0; cycle < 4; cycle++)
    {
        // Each open starts from the first scripted attempt again.
        g_connect_attempt_count = 0;
        g_last_addrinfo_family = -1;
        g_open_complete_count = 0;
        g_open_result.result = IO_OPEN_CANCELLED;

        ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

        ASSERT_ARE_EQUAL(int, AF_UNSPEC, g_last_addrinfo_family);
        ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
        ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
        ASSERT_ARE_EQUAL(int, AF_INET6, g_connect_families[0]);

        ASSERT_ARE_EQUAL(int, 0, socketio_close(ioHandle, NULL, NULL));
        ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());
    }

    socketio_destroy(ioHandle);
}

/* However many candidates are tried, the caller hears about the open once. */
TEST_FUNCTION(socketio_open_reports_one_open_complete_when_a_later_candidate_connects)
{
    const int families[] = { AF_INET6, AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_TIMES_OUT, ATTEMPT_REFUSED, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(3, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)3, g_connect_attempt_count);

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_reports_one_open_complete_when_every_candidate_fails)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_REFUSED, ATTEMPT_TIMES_OUT };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());

    socketio_destroy(ioHandle);
}

/* A failed lookup has no candidate to try: one error callback carrying the
   resolver's code, and no socket created. */
TEST_FUNCTION(socketio_open_dns_failure_reports_one_error_and_creates_no_socket)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(1, families, outcomes);
    g_getaddrinfo_result = EAI_NONAME;
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    ASSERT_ARE_EQUAL(int, EAI_NONAME, g_open_result.code);
    ASSERT_ARE_EQUAL(size_t, (size_t)0, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());

    socketio_destroy(ioHandle);
}

/* A failed open leaves the instance closed and clean, so it can be opened again. */
TEST_FUNCTION(socketio_open_can_be_retried_after_every_candidate_failed)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME fail[] = { ATTEMPT_TIMES_OUT };
    const ATTEMPT_OUTCOME succeed[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;

    given_candidates(1, families, fail);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);

    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);

    g_connect_attempt_count = 0;
    g_poll_count = 0;
    given_candidates(1, families, succeed);
    (void)socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL);

    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, EXPECTED_PER_ADDRESS_TIMEOUT_MS, g_poll_timeouts_ms[0]);

    socketio_destroy(ioHandle);
}

/* A signal landing while the adapter waits for the connect must not fail the
   candidate: poll() is retried after EINTR and the connect completes. */
TEST_FUNCTION(socketio_open_retries_poll_after_eintr_and_connects)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_EINTR_THEN_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_connect_attempt_count);
    // One interrupted wait and one completed wait on the same candidate.
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_poll_count);
    // The connected socket is kept open by the instance.
    ASSERT_ARE_EQUAL(size_t, fds_before + 1, open_fd_count());

    socketio_destroy(ioHandle);
    ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());
}

/* poll() failing outright (not a timeout, not EINTR) fails that candidate with
   poll's errno, releases its socket and lets the next candidate be tried. */
TEST_FUNCTION(socketio_open_poll_failure_fails_the_candidate_and_falls_back)
{
    const int families[] = { AF_INET6, AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_POLL_FAILS, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(int, AF_INET, g_connect_families[1]);
    // Only the socket of the candidate that connected is still held.
    ASSERT_ARE_EQUAL(size_t, fds_before + 1, open_fd_count());

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_poll_failure_on_the_only_candidate_reports_poll_errno)
{
    const int families[] = { AF_INET };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_POLL_FAILS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    ASSERT_ARE_EQUAL(int, EBADF, g_open_result.code);
    ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());

    socketio_destroy(ioHandle);
}

/* Malformed resolver entries are rejected before a socket is created for them,
   and a later valid candidate still gets its attempt. */
TEST_FUNCTION(socketio_open_skips_invalid_resolved_addresses_before_creating_a_socket)
{
    const int families[] = { CANDIDATE_INVALID_FAMILY, CANDIDATE_SHORT_ADDRLEN, AF_INET };
    // Outcomes are consumed per connect attempt; only the valid entry connects.
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS, ATTEMPT_SUCCEEDS, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(3, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(int, IO_OPEN_OK, g_open_result.result);
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_socket_count);
    ASSERT_ARE_EQUAL(int, AF_INET, g_socket_domains[0]);
    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(size_t, fds_before + 1, open_fd_count());

    socketio_destroy(ioHandle);
}

TEST_FUNCTION(socketio_open_with_only_invalid_resolved_addresses_fails_without_a_socket)
{
    const int families[] = { CANDIDATE_INVALID_FAMILY, CANDIDATE_SHORT_ADDRLEN };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS, ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(2, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    ASSERT_ARE_EQUAL(size_t, (size_t)1, g_open_complete_count);
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    // The last rejection reason is reported (EINVAL for the short address).
    ASSERT_ARE_EQUAL(int, EINVAL, g_open_result.code);
    ASSERT_ARE_EQUAL(size_t, (size_t)0, g_socket_count);
    ASSERT_ARE_EQUAL(size_t, (size_t)0, g_connect_attempt_count);
    ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());

    socketio_destroy(ioHandle);
}

#ifndef __APPLE__
/* SIOCGIFCONF only works on an AF_INET socket, so binding an AF_INET6 connect
   socket to an interface enumerates on a temporary AF_INET datagram socket -
   which must be released whatever the enumeration's outcome. */
TEST_FUNCTION(socketio_open_ipv6_interface_binding_enumerates_on_a_temporary_ipv4_socket)
{
    const int families[] = { AF_INET6 };
    const ATTEMPT_OUTCOME outcomes[] = { ATTEMPT_SUCCEEDS };
    CONCRETE_IO_HANDLE ioHandle;
    size_t fds_before;

    given_candidates(1, families, outcomes);
    ioHandle = create_socket_io(HOSTNAME_ARG, 1);
    ASSERT_ARE_EQUAL(int, 0, socketio_setoption(ioHandle, OPTION_NET_INT_MAC_ADDRESS, "00:11:22:33:44:55"));
    fds_before = open_fd_count();

    ASSERT_ARE_EQUAL(int, 0, socketio_open(ioHandle, test_on_io_open_complete, NULL, test_on_bytes_received, NULL, test_on_io_error, NULL));

    // The mocked descriptors are /dev/null, so the enumeration ioctl fails with
    // ENOTTY; that platform error is what must be reported.
    ASSERT_ARE_EQUAL(int, IO_OPEN_ERROR, g_open_result.result);
    ASSERT_ARE_EQUAL(int, ENOTTY, g_open_result.code);
    ASSERT_ARE_EQUAL(size_t, (size_t)2, g_socket_count);
    ASSERT_ARE_EQUAL(int, AF_INET6, g_socket_domains[0]);
    ASSERT_ARE_EQUAL(int, SOCK_STREAM, g_socket_types[0]);
    ASSERT_ARE_EQUAL(int, AF_INET, g_socket_domains[1]);
    ASSERT_ARE_EQUAL(int, SOCK_DGRAM, g_socket_types[1]);
    // Both the connect socket and the enumeration socket were released.
    ASSERT_ARE_EQUAL(size_t, fds_before, open_fd_count());

    socketio_destroy(ioHandle);
}
#endif

END_TEST_SUITE(socketio_berkeley_unittests)
