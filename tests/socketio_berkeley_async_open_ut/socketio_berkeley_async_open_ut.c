// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int test_getaddrinfo(
    const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** result);
static void test_freeaddrinfo(struct addrinfo* result);
static int test_socket(int domain, int type, int protocol);
static int test_connect(int socket, const struct sockaddr* address, socklen_t address_length);
static int test_clock_gettime(clockid_t clock_id, struct timespec* current_time);

#undef _DEFAULT_SOURCE
#define SOCKETIO_BERKELEY_GETADDRINFO test_getaddrinfo
#define SOCKETIO_BERKELEY_FREEADDRINFO test_freeaddrinfo
#define SOCKETIO_BERKELEY_SOCKET test_socket
#define SOCKETIO_BERKELEY_CONNECT test_connect
#define SOCKETIO_BERKELEY_CLOCK_GETTIME test_clock_gettime
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

typedef struct CALLBACK_STATE_TAG
{
    int callback_sequence;
    int open_count;
    int close_count;
    int open_callback_order;
    int close_callback_order;
    IO_OPEN_RESULT_DETAILED last_open_result;
} CALLBACK_STATE;

static int mock_peer_socket = INVALID_SOCKET;
static uint64_t mock_time_ms = 1000;

static int test_getaddrinfo(
    const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** result)
{
    (void)node;
    (void)service;
    return getaddrinfo("127.0.0.1", "1", hints, result);
}

static void test_freeaddrinfo(struct addrinfo* result)
{
    freeaddrinfo(result);
}

static int test_socket(int domain, int type, int protocol)
{
    int sockets[2];
    (void)domain;
    (void)type;
    (void)protocol;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    {
        return INVALID_SOCKET;
    }

    if (mock_peer_socket != INVALID_SOCKET)
    {
        close(mock_peer_socket);
    }
    mock_peer_socket = sockets[1];
    return sockets[0];
}

static int test_connect(int socket, const struct sockaddr* address, socklen_t address_length)
{
    (void)socket;
    (void)address;
    (void)address_length;
    errno = EINPROGRESS;
    return __FAILURE__;
}

static int test_clock_gettime(clockid_t clock_id, struct timespec* current_time)
{
    TEST_CHECK(clock_id == CLOCK_MONOTONIC);
    current_time->tv_sec = (time_t)(mock_time_ms / 1000);
    current_time->tv_nsec = (long)((mock_time_ms % 1000) * 1000000);
    return 0;
}

static void close_mock_peer(void)
{
    if (mock_peer_socket != INVALID_SOCKET)
    {
        close(mock_peer_socket);
        mock_peer_socket = INVALID_SOCKET;
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

static CONCRETE_IO_HANDLE create_test_socket_io(void)
{
    SOCKETIO_CONFIG config = { "unused.test", 443, NULL };
    return socketio_create(&config);
}

static int open_test_socket_io(CONCRETE_IO_HANDLE socket_io, CALLBACK_STATE* callback_state)
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

static int close_during_opening_cancels_once(void)
{
    CALLBACK_STATE callback_state = { 0 };
    CONCRETE_IO_HANDLE socket_io = create_test_socket_io();

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

    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);

    close_mock_peer();
    socketio_destroy(socket_io);
    return 0;
}

static int failed_open_can_be_reopened_without_close(void)
{
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io = create_test_socket_io();

    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    TEST_CHECK(callback_state.open_count == 0);

    fail_connection_attempt(socket_io, &socket_io->attempts[0], ECONNREFUSED);
    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_ERROR);
    TEST_CHECK(callback_state.last_open_result.code == ECONNREFUSED);
    TEST_CHECK(socket_io->io_state == IO_STATE_CLOSED);

    close_mock_peer();
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(socket_io->io_state == IO_STATE_OPENING);
    TEST_CHECK(socket_io->attempts[0].state == CONNECTION_ATTEMPT_CONNECTING);

    TEST_CHECK(socketio_close(socket_io, NULL, NULL) == 0);
    TEST_CHECK(callback_state.open_count == 2);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_CANCELLED);

    close_mock_peer();
    socketio_destroy(socket_io);
    return 0;
}

static int recorded_success_wins_at_expired_deadline(void)
{
    CALLBACK_STATE callback_state = { 0 };
    SOCKET_IO_INSTANCE* socket_io = create_test_socket_io();
    int winning_socket;

    TEST_CHECK(socket_io != NULL);
    TEST_CHECK(open_test_socket_io(socket_io, &callback_state) == 0);
    TEST_CHECK(callback_state.open_count == 0);

    winning_socket = socket_io->attempts[0].socket;
    socket_io->attempts[0].state = CONNECTION_ATTEMPT_SUCCEEDED;
    socket_io->overall_deadline_ms = mock_time_ms;

    socketio_dowork(socket_io);
    TEST_CHECK(callback_state.open_count == 1);
    TEST_CHECK(callback_state.last_open_result.result == IO_OPEN_OK);
    TEST_CHECK(callback_state.last_open_result.code == 0);
    TEST_CHECK(socket_io->io_state == IO_STATE_OPEN);
    TEST_CHECK(socket_io->socket == winning_socket);

    close_mock_peer();
    socketio_destroy(socket_io);
    return 0;
}

static int candidate_order_alternates_after_preferred_family(void)
{
    struct sockaddr_in6 ipv6_addresses[2] = { 0 };
    struct sockaddr_in ipv4_addresses[2] = { 0 };
    struct addrinfo addresses[4] = { 0 };
    SOCKET_IO_INSTANCE* socket_io = create_test_socket_io();

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

    free(socket_io->candidates);
    socket_io->candidates = NULL;
    socket_io->address_list = NULL;
    socketio_destroy(socket_io);
    return 0;
}

int main(void)
{
    int result;

    result = close_during_opening_cancels_once();
    if (result == 0)
    {
        result = failed_open_can_be_reopened_without_close();
    }
    if (result == 0)
    {
        result = recorded_success_wins_at_expired_deadline();
    }
    if (result == 0)
    {
        result = candidate_order_alternates_after_preferred_family();
    }

    close_mock_peer();
    if (result == 0)
    {
        (void)printf("socketio_berkeley_async_open_ut passed\n");
    }
    return result;
}
