// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define MOCK_MAX_CANDIDATES 8
#define MOCK_SOCKET_BASE 100

static int test_getaddrinfo(
    const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** result);
static void test_freeaddrinfo(struct addrinfo* result);
static int test_socket(int domain, int type, int protocol);
static int test_connect(int socket, const struct sockaddr* address, socklen_t address_length);
static int test_clock_gettime(clockid_t clock_id, struct timespec* current_time);
static int test_poll(struct pollfd* descriptors, nfds_t descriptor_count, int timeout);
static int test_getsockopt(
    int socket, int level, int option_name, void* option_value, socklen_t* option_length);
static int test_close(int socket);
static int test_fcntl_getfl(int socket);
static int test_fcntl_setfl(int socket, int flags);
static void* test_race_malloc(size_t size);
static void test_race_free(void* pointer);

#undef _DEFAULT_SOURCE
#define SOCKETIO_BERKELEY_GETADDRINFO test_getaddrinfo
#define SOCKETIO_BERKELEY_FREEADDRINFO test_freeaddrinfo
#define SOCKETIO_BERKELEY_SOCKET test_socket
#define SOCKETIO_BERKELEY_CONNECT test_connect
#define SOCKETIO_BERKELEY_CLOCK_GETTIME test_clock_gettime
#define SOCKETIO_BERKELEY_POLL test_poll
#define SOCKETIO_BERKELEY_GETSOCKOPT test_getsockopt
#define SOCKETIO_BERKELEY_CLOSE test_close
#define SOCKETIO_BERKELEY_FCNTL_GETFL test_fcntl_getfl
#define SOCKETIO_BERKELEY_FCNTL_SETFL test_fcntl_setfl
#define SOCKETIO_BERKELEY_RACE_MALLOC test_race_malloc
#define SOCKETIO_BERKELEY_RACE_FREE test_race_free
#include "../../adapters/socketio_berkeley.c"

#define TEST_CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            (void)fprintf(stderr, "Test failure at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return __LINE__; \
        } \
    } while (0)

typedef enum MOCK_CONNECT_RESULT_TAG
{
    MOCK_CONNECT_PENDING,
    MOCK_CONNECT_SUCCEEDS,
    MOCK_CONNECT_FAILS
} MOCK_CONNECT_RESULT;

typedef struct CALLBACK_STATE_TAG
{
    int callback_sequence;
    int open_count;
    int close_count;
    int open_callback_order;
    int close_callback_order;
    IO_OPEN_RESULT_DETAILED last_open_result;
} CALLBACK_STATE;

static struct addrinfo mock_addresses[MOCK_MAX_CANDIDATES];
static struct sockaddr_storage mock_socket_addresses[MOCK_MAX_CANDIDATES];
static size_t mock_candidate_count;
static uint64_t mock_time_ms;
static int mock_socket_count;
static int mock_socket_families[MOCK_MAX_CANDIDATES];
static MOCK_CONNECT_RESULT mock_connect_results[MOCK_MAX_CANDIDATES];
static int mock_connect_errors[MOCK_MAX_CANDIDATES];
static int mock_poll_ready[MOCK_MAX_CANDIDATES];
static int mock_socket_errors[MOCK_MAX_CANDIDATES];
static int mock_close_counts[MOCK_MAX_CANDIDATES];
static int mock_poll_call_count;
static nfds_t mock_max_polled_descriptor_count;
static int mock_freeaddrinfo_count;
static int mock_race_allocation_call_count;
static int mock_race_allocation_fail_call;
static int mock_race_outstanding_allocation_count;
static int mock_race_allocation_tracking_error;

#define TEST_CHECK_RACE_ALLOCATIONS_RELEASED() \
    do \
    { \
        TEST_CHECK(mock_race_outstanding_allocation_count == 0); \
        TEST_CHECK(mock_race_allocation_tracking_error == 0); \
    } while (0)

static int socket_index_from_descriptor(int socket)
{
    return socket - MOCK_SOCKET_BASE;
}

static void reset_mocks(void)
{
    (void)memset(mock_addresses, 0, sizeof(mock_addresses));
    (void)memset(mock_socket_addresses, 0, sizeof(mock_socket_addresses));
    (void)memset(mock_socket_families, 0, sizeof(mock_socket_families));
    (void)memset(mock_connect_results, 0, sizeof(mock_connect_results));
    (void)memset(mock_connect_errors, 0, sizeof(mock_connect_errors));
    (void)memset(mock_poll_ready, 0, sizeof(mock_poll_ready));
    (void)memset(mock_socket_errors, 0, sizeof(mock_socket_errors));
    (void)memset(mock_close_counts, 0, sizeof(mock_close_counts));
    mock_candidate_count = 0;
    mock_time_ms = 1000;
    mock_socket_count = 0;
    mock_poll_call_count = 0;
    mock_max_polled_descriptor_count = 0;
    mock_freeaddrinfo_count = 0;
    mock_race_allocation_call_count = 0;
    mock_race_allocation_fail_call = 0;
}

static void set_mock_candidates(const int* families, size_t family_count)
{
    size_t candidate_index;

    mock_candidate_count = family_count;
    for (candidate_index = 0; candidate_index < family_count; candidate_index++)
    {
        mock_addresses[candidate_index].ai_family = families[candidate_index];
        mock_addresses[candidate_index].ai_socktype = SOCK_STREAM;
        mock_addresses[candidate_index].ai_addr = (struct sockaddr*)&mock_socket_addresses[candidate_index];
        mock_addresses[candidate_index].ai_addrlen =
            (families[candidate_index] == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        mock_addresses[candidate_index].ai_next =
            (candidate_index + 1 < family_count) ? &mock_addresses[candidate_index + 1] : NULL;
        mock_socket_addresses[candidate_index].ss_family = (sa_family_t)families[candidate_index];
    }
}

static int test_getaddrinfo(
    const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** result)
{
    (void)node;
    (void)service;
    (void)hints;
    *result = (mock_candidate_count == 0) ? NULL : &mock_addresses[0];
    return (mock_candidate_count == 0) ? EAI_NONAME : 0;
}

static void test_freeaddrinfo(struct addrinfo* result)
{
    (void)result;
    mock_freeaddrinfo_count++;
}

static int test_socket(int domain, int type, int protocol)
{
    int socket_index = mock_socket_count++;
    (void)type;
    (void)protocol;

    if (socket_index >= MOCK_MAX_CANDIDATES)
    {
        errno = EMFILE;
        return INVALID_SOCKET;
    }

    mock_socket_families[socket_index] = domain;
    return MOCK_SOCKET_BASE + socket_index;
}

static int test_connect(int socket, const struct sockaddr* address, socklen_t address_length)
{
    int socket_index = socket_index_from_descriptor(socket);
    (void)address;
    (void)address_length;

    if (mock_connect_results[socket_index] == MOCK_CONNECT_SUCCEEDS)
    {
        return 0;
    }
    else if (mock_connect_results[socket_index] == MOCK_CONNECT_FAILS)
    {
        errno = (mock_connect_errors[socket_index] == 0) ? ECONNREFUSED : mock_connect_errors[socket_index];
        return __FAILURE__;
    }
    else
    {
        errno = EINPROGRESS;
        return __FAILURE__;
    }
}

static int test_clock_gettime(clockid_t clock_id, struct timespec* current_time)
{
    if (clock_id != CLOCK_MONOTONIC)
    {
        errno = EINVAL;
        return __FAILURE__;
    }

    current_time->tv_sec = (time_t)(mock_time_ms / 1000);
    current_time->tv_nsec = (long)((mock_time_ms % 1000) * 1000000);
    return 0;
}

static int test_poll(struct pollfd* descriptors, nfds_t descriptor_count, int timeout)
{
    nfds_t descriptor_index;
    int ready_count = 0;

    mock_poll_call_count++;
    if (descriptor_count > mock_max_polled_descriptor_count)
    {
        mock_max_polled_descriptor_count = descriptor_count;
    }
    if (timeout != 0)
    {
        errno = EINVAL;
        return __FAILURE__;
    }

    for (descriptor_index = 0; descriptor_index < descriptor_count; descriptor_index++)
    {
        int socket_index = socket_index_from_descriptor(descriptors[descriptor_index].fd);
        descriptors[descriptor_index].revents = 0;
        if (mock_poll_ready[socket_index] != 0)
        {
            descriptors[descriptor_index].revents = POLLOUT;
            ready_count++;
        }
    }

    return ready_count;
}

static int test_getsockopt(
    int socket, int level, int option_name, void* option_value, socklen_t* option_length)
{
    int socket_index = socket_index_from_descriptor(socket);

    if ((level != SOL_SOCKET) || (option_name != SO_ERROR) ||
        (option_value == NULL) || (option_length == NULL) || (*option_length < sizeof(int)))
    {
        errno = EINVAL;
        return __FAILURE__;
    }

    *(int*)option_value = mock_socket_errors[socket_index];
    *option_length = sizeof(int);
    return 0;
}

static int test_close(int socket)
{
    int socket_index = socket_index_from_descriptor(socket);
    if ((socket_index >= 0) && (socket_index < MOCK_MAX_CANDIDATES))
    {
        mock_close_counts[socket_index]++;
    }
    return 0;
}

static int test_fcntl_getfl(int socket)
{
    (void)socket;
    return 0;
}

static int test_fcntl_setfl(int socket, int flags)
{
    (void)socket;
    (void)flags;
    return 0;
}

static void* test_race_malloc(size_t size)
{
    void* result;

    mock_race_allocation_call_count++;
    if (mock_race_allocation_fail_call == mock_race_allocation_call_count)
    {
        result = NULL;
    }
    else
    {
        result = malloc(size);
        if (result != NULL)
        {
            mock_race_outstanding_allocation_count++;
        }
    }

    return result;
}

static void test_race_free(void* pointer)
{
    if (pointer != NULL)
    {
        if (mock_race_outstanding_allocation_count == 0)
        {
            mock_race_allocation_tracking_error = 1;
        }
        else
        {
            mock_race_outstanding_allocation_count--;
        }
        free(pointer);
    }
}

static void on_open(void* context, IO_OPEN_RESULT_DETAILED open_result)
{
    CALLBACK_STATE* callback_state = context;
    callback_state->open_count++;
    callback_state->open_callback_order = ++callback_state->callback_sequence;
    callback_state->last_open_result = open_result;
}

static void on_close(void* context)
{
    CALLBACK_STATE* callback_state = context;
    callback_state->close_count++;
    callback_state->close_callback_order = ++callback_state->callback_sequence;
}

static void on_bytes_received(void* context, const unsigned char* buffer, size_t size)
{
    (void)context;
    (void)buffer;
    (void)size;
}

static void on_io_error(void* context)
{
    (void)context;
}

static SOCKET_IO_INSTANCE* create_test_socket_io(void)
{
    SOCKETIO_CONFIG config = { "unused.test", 443, NULL };
    return (SOCKET_IO_INSTANCE*)socketio_create(&config);
}

static int open_test_socket_io(SOCKET_IO_INSTANCE* socket_io, CALLBACK_STATE* callback_state)
{
    return socketio_open(
        socket_io,
        on_open,
        callback_state,
        on_bytes_received,
        callback_state,
        on_io_error,
        callback_state);
}

static int start_pending_attempts(
    SOCKET_IO_INSTANCE* socket_io, CALLBACK_STATE* callback_state, const int* families, size_t candidate_count)
{
    size_t candidate_index;

    set_mock_candidates(families, candidate_count);
    TEST_CHECK(open_test_socket_io(socket_io, callback_state) == 0);
    TEST_CHECK(socket_io->active_attempt_count == 1);
    TEST_CHECK(mock_socket_count == 1);

    for (candidate_index = 1; candidate_index < candidate_count; candidate_index++)
    {
        mock_time_ms += CONNECTION_ATTEMPT_DELAY_MS;
        socketio_dowork(socket_io);
        TEST_CHECK(socket_io->active_attempt_count == candidate_index + 1);
        TEST_CHECK(mock_socket_count == (int)(candidate_index + 1));
    }
    return 0;
}

static int start_two_pending_attempts(
    SOCKET_IO_INSTANCE* socket_io, CALLBACK_STATE* callback_state, const int* families)
{
    return start_pending_attempts(socket_io, callback_state, families, 2);
}

static void destroy_after_success(SOCKET_IO_INSTANCE* socket_io)
{
    socket_io->socket = INVALID_SOCKET;
    socketio_destroy(socket_io);
}

static int close_during_opening_cancels_once(void)
{
    const int families[] = { AF_INET6 };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;

    reset_mocks();
    set_mock_candidates(families, 1);
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    TEST_CHECK(callback_state.open_count == 0);

    TEST_CHECK(socketio_close(socket_io, on_close, &callback_state) == 0);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_CANCELLED);
    TEST_CHECK(callback_state.last_open_result.code == 0);
    TEST_CHECK(callback_state.close_count == 1);
    TEST_CHECK(callback_state.open_callback_order == 1);
    TEST_CHECK(callback_state.close_callback_order == 2);
    TEST_CHECK(mock_close_counts[0] == 1);

    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(mock_close_counts[0] == 1);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int failed_open_can_be_reopened_without_close(void)
{
    const int families[] = { AF_INET6 };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;

    reset_mocks();
    set_mock_candidates(families, 1);
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);

    mock_poll_ready[0] = 1;
    mock_socket_errors[0] = ECONNREFUSED;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_ERROR);
    TEST_CHECK(callback_state.last_open_result.code == ECONNREFUSED);
    TEST_CHECK(socket_io->io_state == IO_STATE_CLOSED);
    TEST_CHECK(mock_close_counts[0] == 1);

    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(socket_io->io_state == IO_STATE_OPENING);
    TEST_CHECK(socket_io->active_attempt_count == 1);
    TEST_CHECK(mock_socket_count == 2);

    TEST_CHECK(socketio_close(socket_io, NULL, NULL) == 0);
    TEST_CHECK(callback_state.open_count == 2);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_CANCELLED);
    TEST_CHECK(mock_close_counts[1] == 1);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int recorded_success_wins_at_expired_deadline(void)
{
    const int families[] = { AF_INET6 };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    int winning_socket;

    reset_mocks();
    set_mock_candidates(families, 1);
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);

    winning_socket = socket_io->attempts[0].socket;
    socket_io->attempts[0].state = CONNECTION_ATTEMPT_SUCCEEDED;
    socket_io->overall_deadline_ms = mock_time_ms;
    socketio_dowork(socket_io);

    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_OK);
    TEST_CHECK(socket_io->io_state == IO_STATE_OPEN);
    TEST_CHECK(socket_io->socket == winning_socket);
    TEST_CHECK(mock_poll_call_count == 0);
    TEST_CHECK(mock_close_counts[0] == 0);
    destroy_after_success(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int four_pending_attempts_start_at_fixed_intervals(void)
{
    const int families[] = { AF_INET6, AF_INET, AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    size_t candidate_index;

    reset_mocks();
    set_mock_candidates(families, 4);
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    TEST_CHECK(mock_socket_count == 1);
    TEST_CHECK(socket_io->attempts[0].started_at_ms == 1000);

    mock_time_ms = 1000 + CONNECTION_ATTEMPT_DELAY_MS - 1;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 1);
    TEST_CHECK(socket_io->active_attempt_count == 1);

    mock_time_ms++;
    socketio_dowork(socket_io);
    TEST_CHECK(CONNECTION_ATTEMPT_DELAY_MS == 250);
    TEST_CHECK(CONNECTION_ATTEMPT_DELAY_MS >= 100);
    TEST_CHECK(mock_socket_count == 2);
    TEST_CHECK(socket_io->active_attempt_count == 2);
    TEST_CHECK(socket_io->attempts[1].started_at_ms - socket_io->attempts[0].started_at_ms ==
        CONNECTION_ATTEMPT_DELAY_MS);

    mock_time_ms += CONNECTION_ATTEMPT_DELAY_MS;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 3);
    TEST_CHECK(socket_io->active_attempt_count == 3);
    TEST_CHECK(socket_io->attempts[2].started_at_ms == 1500);

    mock_time_ms += CONNECTION_ATTEMPT_DELAY_MS;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 4);
    TEST_CHECK(socket_io->active_attempt_count == 4);
    TEST_CHECK(socket_io->attempts[3].started_at_ms == 1750);
    socketio_dowork(socket_io);
    TEST_CHECK(mock_max_polled_descriptor_count == 4);

    for (candidate_index = 0; candidate_index < 4; candidate_index++)
    {
        TEST_CHECK(mock_socket_families[candidate_index] == families[candidate_index]);
    }
    socketio_destroy(socket_io);
    for (candidate_index = 0; candidate_index < 4; candidate_index++)
    {
        TEST_CHECK(mock_close_counts[candidate_index] == 1);
    }
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int delayed_dowork_starts_only_one_attempt(void)
{
    const int families[] = { AF_INET6, AF_INET, AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;

    reset_mocks();
    set_mock_candidates(families, 4);
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);

    mock_time_ms = 5000;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 2);
    TEST_CHECK(socket_io->attempts[1].started_at_ms == 5000);
    TEST_CHECK(socket_io->next_attempt_at_ms == 5000 + CONNECTION_ATTEMPT_DELAY_MS);

    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 2);
    mock_time_ms += CONNECTION_ATTEMPT_DELAY_MS - 1;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 2);

    mock_time_ms++;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 3);
    TEST_CHECK(socket_io->attempts[2].started_at_ms == 5250);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int third_candidate_wins_before_fourth_starts(void)
{
    const int families[] = { AF_INET6, AF_INET, AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;

    reset_mocks();
    set_mock_candidates(families, 4);
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    mock_time_ms += CONNECTION_ATTEMPT_DELAY_MS;
    socketio_dowork(socket_io);
    mock_time_ms += CONNECTION_ATTEMPT_DELAY_MS;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 3);
    TEST_CHECK(socket_io->active_attempt_count == 3);

    mock_poll_ready[2] = 1;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_OK);
    TEST_CHECK(socket_io->socket == MOCK_SOCKET_BASE + 2);
    TEST_CHECK(mock_socket_count == 3);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_close_counts[1] == 1);
    TEST_CHECK(mock_close_counts[2] == 0);
    TEST_CHECK(mock_close_counts[3] == 0);
    destroy_after_success(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int ipv4_winner_closes_ipv6_loser(void)
{
    const int families[] = { AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    int result;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    result = start_two_pending_attempts(socket_io, &callback_state, families);
    TEST_CHECK(result == 0);

    mock_poll_ready[1] = 1;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_OK);
    TEST_CHECK(socket_io->socket == MOCK_SOCKET_BASE + 1);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_close_counts[1] == 0);
    destroy_after_success(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int ipv6_winner_closes_ipv4_loser(void)
{
    const int families[] = { AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    int result;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    result = start_two_pending_attempts(socket_io, &callback_state, families);
    TEST_CHECK(result == 0);

    mock_poll_ready[0] = 1;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_OK);
    TEST_CHECK(socket_io->socket == MOCK_SOCKET_BASE);
    TEST_CHECK(mock_close_counts[0] == 0);
    TEST_CHECK(mock_close_counts[1] == 1);
    destroy_after_success(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int simultaneous_successes_choose_lowest_of_four_candidates(void)
{
    const int families[] = { AF_INET6, AF_INET, AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    int result;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    result = start_pending_attempts(socket_io, &callback_state, families, 4);
    TEST_CHECK(result == 0);

    mock_poll_ready[1] = 1;
    mock_poll_ready[2] = 1;
    mock_poll_ready[3] = 1;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_OK);
    TEST_CHECK(socket_io->socket == MOCK_SOCKET_BASE + 1);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_close_counts[1] == 0);
    TEST_CHECK(mock_close_counts[2] == 1);
    TEST_CHECK(mock_close_counts[3] == 1);
    TEST_CHECK(mock_max_polled_descriptor_count == 4);
    destroy_after_success(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int immediate_failure_respects_delay_then_later_candidate_succeeds(void)
{
    const int families[] = { AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;

    reset_mocks();
    set_mock_candidates(families, 2);
    mock_connect_results[0] = MOCK_CONNECT_FAILS;
    mock_connect_errors[0] = ENETUNREACH;
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_socket_count == 1);
    TEST_CHECK(socket_io->active_attempt_count == 0);

    mock_time_ms = 1000 + CONNECTION_ATTEMPT_DELAY_MS - 1;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 1);
    TEST_CHECK(socket_io->active_attempt_count == 0);

    mock_time_ms++;
    socketio_dowork(socket_io);
    TEST_CHECK(mock_socket_count == 2);
    TEST_CHECK(socket_io->active_attempt_count == 1);
    TEST_CHECK(socket_io->attempts[0].candidate_index == 1);
    TEST_CHECK(socket_io->attempts[0].started_at_ms == 1250);

    mock_poll_ready[1] = 1;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_OK);
    TEST_CHECK(socket_io->socket == MOCK_SOCKET_BASE + 1);
    destroy_after_success(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int all_candidates_fail_once(void)
{
    const int families[] = { AF_INET6, AF_INET, AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    int result;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    result = start_pending_attempts(socket_io, &callback_state, families, 4);
    TEST_CHECK(result == 0);

    mock_poll_ready[0] = 1;
    mock_poll_ready[1] = 1;
    mock_poll_ready[2] = 1;
    mock_poll_ready[3] = 1;
    mock_socket_errors[0] = ECONNREFUSED;
    mock_socket_errors[1] = ENETUNREACH;
    mock_socket_errors[2] = EHOSTUNREACH;
    mock_socket_errors[3] = ECONNRESET;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_ERROR);
    TEST_CHECK(callback_state.last_open_result.code == ECONNRESET);
    TEST_CHECK(socket_io->io_state == IO_STATE_CLOSED);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_close_counts[1] == 1);
    TEST_CHECK(mock_close_counts[2] == 1);
    TEST_CHECK(mock_close_counts[3] == 1);

    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int deadline_closes_all_active_attempts(void)
{
    const int families[] = { AF_INET6, AF_INET, AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    int result;
    int poll_call_count_before_deadline;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    result = start_pending_attempts(socket_io, &callback_state, families, 4);
    TEST_CHECK(result == 0);

    mock_poll_ready[0] = 1;
    mock_poll_ready[1] = 1;
    mock_poll_ready[2] = 1;
    mock_poll_ready[3] = 1;
    poll_call_count_before_deadline = mock_poll_call_count;
    mock_time_ms = socket_io->overall_deadline_ms;
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_ERROR);
    TEST_CHECK(callback_state.last_open_result.code == ETIMEDOUT);
    TEST_CHECK(socket_io->io_state == IO_STATE_CLOSED);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_close_counts[1] == 1);
    TEST_CHECK(mock_close_counts[2] == 1);
    TEST_CHECK(mock_close_counts[3] == 1);
    TEST_CHECK(mock_poll_call_count == poll_call_count_before_deadline);
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int close_and_destroy_close_four_attempts(void)
{
    const int families[] = { AF_INET6, AF_INET, AF_INET6, AF_INET };
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io;
    int result;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    result = start_pending_attempts(socket_io, &callback_state, families, 4);
    TEST_CHECK(result == 0);
    TEST_CHECK(socketio_close(socket_io, on_close, &callback_state) == 0);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_CANCELLED);
    TEST_CHECK(callback_state.close_count == 1);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_close_counts[1] == 1);
    TEST_CHECK(mock_close_counts[2] == 1);
    TEST_CHECK(mock_close_counts[3] == 1);
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();

    reset_mocks();
    (void)memset(&callback_state, 0, sizeof(callback_state));
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    result = start_pending_attempts(socket_io, &callback_state, families, 4);
    TEST_CHECK(result == 0);
    socketio_destroy(socket_io);
    TEST_CHECK(callback_state.open_count == 0);
    TEST_CHECK(mock_close_counts[0] == 1);
    TEST_CHECK(mock_close_counts[1] == 1);
    TEST_CHECK(mock_close_counts[2] == 1);
    TEST_CHECK(mock_close_counts[3] == 1);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int every_dynamic_race_allocation_failure_is_reported_and_cleaned_up(void)
{
    const int families[] = { AF_INET6, AF_INET };
    int allocation_fail_call;

    for (allocation_fail_call = 1; allocation_fail_call <= 4; allocation_fail_call++)
    {
        CALLBACK_STATE callback_state = { 0 };
        SOCKET_IO_INSTANCE* socket_io;

        reset_mocks();
        set_mock_candidates(families, 2);
        mock_race_allocation_fail_call = allocation_fail_call;
        socket_io = create_test_socket_io();
        TEST_CHECK(socket_io != NULL);
        TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
        TEST_CHECK(mock_race_allocation_call_count == allocation_fail_call);
        TEST_CHECK(callback_state.open_count == 1);
        TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_ERROR);
        TEST_CHECK(callback_state.last_open_result.code == ENOMEM);
        TEST_CHECK(socket_io->io_state == IO_STATE_CLOSED);
        TEST_CHECK(socket_io->socket == INVALID_SOCKET);
        TEST_CHECK(socket_io->candidate_count == 0);
        TEST_CHECK(socket_io->active_attempt_count == 0);
        TEST_CHECK(socket_io->attempts == NULL);
        TEST_CHECK(socket_io->poll_descriptors == NULL);
        TEST_CHECK(socket_io->polled_attempt_indices == NULL);
        TEST_CHECK(socket_io->candidates == NULL);
        TEST_CHECK(socket_io->address_list == NULL);
        TEST_CHECK(socket_io->on_io_open_complete == NULL);
        TEST_CHECK(socket_io->on_io_open_complete_context == NULL);
        TEST_CHECK(socket_io->on_bytes_received == NULL);
        TEST_CHECK(socket_io->on_bytes_received_context == NULL);
        TEST_CHECK(socket_io->on_io_error == NULL);
        TEST_CHECK(socket_io->on_io_error_context == NULL);
        TEST_CHECK(mock_socket_count == 0);
        TEST_CHECK(mock_freeaddrinfo_count == 1);
        TEST_CHECK_RACE_ALLOCATIONS_RELEASED();

        socketio_dowork(socket_io);
        TEST_CHECK(callback_state.open_count == 1);
        TEST_CHECK(socketio_close(socket_io, NULL, NULL) == 0);
        TEST_CHECK(callback_state.open_count == 1);
        socketio_destroy(socket_io);
        TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    }

    return 0;
}

static int dynamic_attempt_size_overflow_is_rejected(void)
{
    SOCKET_IO_INSTANCE* socket_io;
    size_t unsafe_candidate_count = (SIZE_MAX / sizeof(CONNECTION_ATTEMPT)) + 1;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(allocate_connection_race_arrays(socket_io, unsafe_candidate_count) != 0);
    TEST_CHECK(socket_io->last_connect_error == EOVERFLOW);
    TEST_CHECK(mock_race_allocation_call_count == 0);
    TEST_CHECK(socket_io->attempts == NULL);
    TEST_CHECK(socket_io->poll_descriptors == NULL);
    TEST_CHECK(socket_io->polled_attempt_indices == NULL);
    TEST_CHECK(socket_io->candidates == NULL);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

static int candidate_order_alternates_after_preferred_family(void)
{
    struct sockaddr_in6 ipv6_addresses[2] = { 0 };
    struct sockaddr_in ipv4_addresses[2] = { 0 };
    struct addrinfo addresses[4] = { 0 };
    SOCKET_IO_INSTANCE* socket_io;

    reset_mocks();
    socket_io = create_test_socket_io();
    TEST_CHECK(socket_io != NULL);
    addresses[0].ai_family = AF_INET6;
    addresses[0].ai_addr = (struct sockaddr*)&ipv6_addresses[0];
    addresses[0].ai_next = &addresses[1];
    addresses[1].ai_family = AF_INET6;
    addresses[1].ai_addr = (struct sockaddr*)&ipv6_addresses[1];
    addresses[1].ai_next = &addresses[2];
    addresses[2].ai_family = AF_INET;
    addresses[2].ai_addr = (struct sockaddr*)&ipv4_addresses[0];
    addresses[2].ai_next = &addresses[3];
    addresses[3].ai_family = AF_INET;
    addresses[3].ai_addr = (struct sockaddr*)&ipv4_addresses[1];

    TEST_CHECK(prepare_connection_candidates(socket_io, addresses) == 0);
    TEST_CHECK(socket_io->candidate_count == 4);
    TEST_CHECK(socket_io->candidates[0] == &addresses[0]);
    TEST_CHECK(socket_io->candidates[1] == &addresses[2]);
    TEST_CHECK(socket_io->candidates[2] == &addresses[1]);
    TEST_CHECK(socket_io->candidates[3] == &addresses[3]);

    socket_io->address_list = NULL;
    dispose_connection_race(socket_io);
    socketio_destroy(socket_io);
    TEST_CHECK_RACE_ALLOCATIONS_RELEASED();
    return 0;
}

int main(void)
{
    int result;

    result = close_during_opening_cancels_once();
    if (result == 0) result = failed_open_can_be_reopened_without_close();
    if (result == 0) result = recorded_success_wins_at_expired_deadline();
    if (result == 0) result = four_pending_attempts_start_at_fixed_intervals();
    if (result == 0) result = delayed_dowork_starts_only_one_attempt();
    if (result == 0) result = third_candidate_wins_before_fourth_starts();
    if (result == 0) result = ipv4_winner_closes_ipv6_loser();
    if (result == 0) result = ipv6_winner_closes_ipv4_loser();
    if (result == 0) result = simultaneous_successes_choose_lowest_of_four_candidates();
    if (result == 0) result = immediate_failure_respects_delay_then_later_candidate_succeeds();
    if (result == 0) result = all_candidates_fail_once();
    if (result == 0) result = deadline_closes_all_active_attempts();
    if (result == 0) result = close_and_destroy_close_four_attempts();
    if (result == 0) result = every_dynamic_race_allocation_failure_is_reported_and_cleaned_up();
    if (result == 0) result = dynamic_attempt_size_overflow_is_rejected();
    if (result == 0) result = candidate_order_alternates_after_preferred_family();

    if (result == 0)
    {
        (void)printf("socketio_berkeley_async_open_ut passed\n");
    }
    return result;
}
