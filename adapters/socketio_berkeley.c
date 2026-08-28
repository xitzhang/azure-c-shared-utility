// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#define SOCKETIO_BERKELEY_UNDEF_BSD_SOURCE
#endif

#define _DEFAULT_SOURCE
#include <net/if.h>
#undef _DEFAULT_SOURCE

#ifdef SOCKETIO_BERKELEY_UNDEF_BSD_SOURCE
#undef _BSD_SOURCE
#undef SOCKETIO_BERKELEY_UNDEF_BSD_SOURCE
#endif

#include <signal.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
#include <time.h>
#endif
#include "azure_c_shared_utility/socketio.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#ifdef TIZENRT
#include <net/lwip/tcp.h>
#else
#include <netinet/tcp.h>
#endif
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/gbnetwork.h"
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/optionhandler.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/const_defines.h"
#include "azure_c_shared_utility/safe_math.h"
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SOCKET_SUCCESS                 0
#define INVALID_SOCKET                 -1
#define MAC_ADDRESS_STRING_LENGTH      18

#ifndef IFREQ_BUFFER_SIZE
#define IFREQ_BUFFER_SIZE              1024
#endif

#define CONNECT_TIMEOUT_SECONDS 10
#define SOCKETIO_POLL_TIMEOUT_ERROR 110  /* ETIMEDOUT equivalent for poll timeout */

#ifndef SOCKETIO_BERKELEY_GETADDRINFO
#define SOCKETIO_BERKELEY_GETADDRINFO getaddrinfo
#endif

#ifndef SOCKETIO_BERKELEY_FREEADDRINFO
#define SOCKETIO_BERKELEY_FREEADDRINFO freeaddrinfo
#endif

#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
#define CONNECTION_ATTEMPT_DELAY_MS 250

#ifndef SOCKETIO_BERKELEY_SOCKET
#define SOCKETIO_BERKELEY_SOCKET socket
#endif

#ifndef SOCKETIO_BERKELEY_CONNECT
#define SOCKETIO_BERKELEY_CONNECT connect
#endif

#ifndef SOCKETIO_BERKELEY_CLOCK_GETTIME
#define SOCKETIO_BERKELEY_CLOCK_GETTIME clock_gettime
#endif

#ifndef SOCKETIO_BERKELEY_POLL
#define SOCKETIO_BERKELEY_POLL poll
#endif

#ifndef SOCKETIO_BERKELEY_GETSOCKOPT
#define SOCKETIO_BERKELEY_GETSOCKOPT getsockopt
#endif

#ifndef SOCKETIO_BERKELEY_CLOSE
#define SOCKETIO_BERKELEY_CLOSE close
#endif

#ifndef SOCKETIO_BERKELEY_FCNTL_GETFL
#define SOCKETIO_BERKELEY_FCNTL_GETFL(socket) fcntl((socket), F_GETFL, 0)
#endif

#ifndef SOCKETIO_BERKELEY_FCNTL_SETFL
#define SOCKETIO_BERKELEY_FCNTL_SETFL(socket, flags) fcntl((socket), F_SETFL, (flags))
#endif

#ifndef SOCKETIO_BERKELEY_RACE_MALLOC
#define SOCKETIO_BERKELEY_RACE_MALLOC malloc
#endif

#ifndef SOCKETIO_BERKELEY_RACE_FREE
#define SOCKETIO_BERKELEY_RACE_FREE free
#endif

typedef enum CONNECTION_ATTEMPT_STATE_TAG
{
    CONNECTION_ATTEMPT_NOT_STARTED,
    CONNECTION_ATTEMPT_CONNECTING,
    CONNECTION_ATTEMPT_SUCCEEDED
} CONNECTION_ATTEMPT_STATE;

typedef struct CONNECTION_ATTEMPT_TAG
{
    /* The attempt owns this descriptor until a winner transfers it to SOCKET_IO_INSTANCE. */
    int socket;
    const struct addrinfo* address;
    CONNECTION_ATTEMPT_STATE state;
    uint64_t started_at_ms;
    size_t candidate_index;
} CONNECTION_ATTEMPT;

#endif

typedef enum IO_STATE_TAG
{
    IO_STATE_CLOSED,
    IO_STATE_OPENING,
    IO_STATE_OPEN,
    IO_STATE_CLOSING,
    IO_STATE_ERROR
} IO_STATE;

typedef struct PENDING_SOCKET_IO_TAG
{
    unsigned char* bytes;
    size_t size;
    ON_SEND_COMPLETE on_send_complete;
    void* callback_context;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
} PENDING_SOCKET_IO;

typedef struct SOCKET_IO_INSTANCE_TAG
{
    int socket;
    ON_BYTES_RECEIVED on_bytes_received;
    ON_IO_ERROR on_io_error;
    void* on_bytes_received_context;
    void* on_io_error_context;
    char* hostname;
    int port;
    char* target_mac_address;
    IO_STATE io_state;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
    unsigned char recv_bytes[RECEIVE_BYTES_VALUE];
#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
    struct addrinfo* address_list;
    const struct addrinfo** candidates;
    size_t candidate_count;
    size_t next_candidate_index;
    CONNECTION_ATTEMPT* attempts;
    struct pollfd* poll_descriptors;
    size_t* polled_attempt_indices;
    size_t active_attempt_count;
    uint64_t overall_deadline_ms;
    uint64_t next_attempt_at_ms;
    ON_IO_OPEN_COMPLETE on_io_open_complete;
    void* on_io_open_complete_context;
    int last_connect_error;
#endif
} SOCKET_IO_INSTANCE;

typedef struct NETWORK_INTERFACE_DESCRIPTION_TAG
{
    char* name;
    char* mac_address;
    char* ip_address;
    struct NETWORK_INTERFACE_DESCRIPTION_TAG* next;
} NETWORK_INTERFACE_DESCRIPTION;

#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
static void initialize_connection_race(SOCKET_IO_INSTANCE* socket_io_instance)
{
    socket_io_instance->address_list = NULL;
    socket_io_instance->candidates = NULL;
    socket_io_instance->candidate_count = 0;
    socket_io_instance->next_candidate_index = 0;
    socket_io_instance->attempts = NULL;
    socket_io_instance->poll_descriptors = NULL;
    socket_io_instance->polled_attempt_indices = NULL;
    socket_io_instance->active_attempt_count = 0;
    socket_io_instance->overall_deadline_ms = 0;
    socket_io_instance->next_attempt_at_ms = 0;
    socket_io_instance->on_io_open_complete = NULL;
    socket_io_instance->on_io_open_complete_context = NULL;
    socket_io_instance->last_connect_error = __FAILURE__;
}

static int get_monotonic_time_ms(uint64_t* current_time_ms, int* error_code)
{
    int result;
    struct timespec current_time;

    if (SOCKETIO_BERKELEY_CLOCK_GETTIME(CLOCK_MONOTONIC, &current_time) != 0)
    {
        *error_code = errno;
        LogError("Failure: clock_gettime(CLOCK_MONOTONIC) failed with error %d (%s).",
            *error_code, strerror(*error_code));
        result = __FAILURE__;
    }
    else if ((current_time.tv_sec < 0) ||
        ((uint64_t)current_time.tv_sec > ((UINT64_MAX - (uint64_t)(current_time.tv_nsec / 1000000)) / 1000)))
    {
        *error_code = EOVERFLOW;
        LogError("Failure: CLOCK_MONOTONIC value cannot be represented in milliseconds.");
        result = __FAILURE__;
    }
    else
    {
        *current_time_ms = ((uint64_t)current_time.tv_sec * 1000) +
            (uint64_t)(current_time.tv_nsec / 1000000);
        *error_code = 0;
        result = 0;
    }

    return result;
}

static void dispose_connection_race(SOCKET_IO_INSTANCE* socket_io_instance)
{
    size_t attempt_index;

    for (attempt_index = 0;
        (socket_io_instance->attempts != NULL) && (attempt_index < socket_io_instance->candidate_count);
        attempt_index++)
    {
        if (socket_io_instance->attempts[attempt_index].socket != INVALID_SOCKET)
        {
            SOCKETIO_BERKELEY_CLOSE(socket_io_instance->attempts[attempt_index].socket);
            socket_io_instance->attempts[attempt_index].socket = INVALID_SOCKET;
        }
    }

    SOCKETIO_BERKELEY_RACE_FREE(socket_io_instance->polled_attempt_indices);
    SOCKETIO_BERKELEY_RACE_FREE(socket_io_instance->poll_descriptors);
    SOCKETIO_BERKELEY_RACE_FREE(socket_io_instance->attempts);
    SOCKETIO_BERKELEY_RACE_FREE(socket_io_instance->candidates);
    if (socket_io_instance->address_list != NULL)
    {
        SOCKETIO_BERKELEY_FREEADDRINFO(socket_io_instance->address_list);
    }

    initialize_connection_race(socket_io_instance);
}

static const struct addrinfo* take_next_candidate(struct addrinfo** cursor, int family)
{
    const struct addrinfo* result = NULL;

    while (*cursor != NULL)
    {
        struct addrinfo* current = *cursor;
        *cursor = current->ai_next;

        if ((current->ai_family == family) && (current->ai_addr != NULL))
        {
            result = current;
            break;
        }
    }

    return result;
}

static int allocate_connection_race_arrays(SOCKET_IO_INSTANCE* socket_io_instance, size_t candidate_count)
{
    int result;
    size_t candidate_allocation_size;
    size_t attempt_allocation_size;
    size_t poll_allocation_size;
    size_t poll_index_allocation_size;
    nfds_t poll_capacity = (nfds_t)candidate_count;
    const struct addrinfo** candidates = NULL;
    CONNECTION_ATTEMPT* attempts = NULL;
    struct pollfd* poll_descriptors = NULL;
    size_t* polled_attempt_indices = NULL;
    size_t candidate_index;

    candidate_allocation_size = safe_multiply_size_t(candidate_count, sizeof(*candidates));
    attempt_allocation_size = safe_multiply_size_t(candidate_count, sizeof(*attempts));
    poll_allocation_size = safe_multiply_size_t(candidate_count, sizeof(*poll_descriptors));
    poll_index_allocation_size = safe_multiply_size_t(candidate_count, sizeof(*polled_attempt_indices));

    if ((candidate_count == 0) ||
        ((size_t)poll_capacity != candidate_count) ||
        (candidate_allocation_size == SIZE_MAX) ||
        (attempt_allocation_size == SIZE_MAX) ||
        (poll_allocation_size == SIZE_MAX) ||
        (poll_index_allocation_size == SIZE_MAX))
    {
        LogError("Failure: connection race candidate count cannot be represented safely.");
        socket_io_instance->last_connect_error = EOVERFLOW;
        result = __FAILURE__;
    }
    else if (((candidates = SOCKETIO_BERKELEY_RACE_MALLOC(candidate_allocation_size)) == NULL) ||
        ((attempts = SOCKETIO_BERKELEY_RACE_MALLOC(attempt_allocation_size)) == NULL) ||
        ((poll_descriptors = SOCKETIO_BERKELEY_RACE_MALLOC(poll_allocation_size)) == NULL) ||
        ((polled_attempt_indices = SOCKETIO_BERKELEY_RACE_MALLOC(poll_index_allocation_size)) == NULL))
    {
        LogError("Failure: unable to allocate connection race state for %zu candidates.", candidate_count);
        SOCKETIO_BERKELEY_RACE_FREE(polled_attempt_indices);
        SOCKETIO_BERKELEY_RACE_FREE(poll_descriptors);
        SOCKETIO_BERKELEY_RACE_FREE(attempts);
        SOCKETIO_BERKELEY_RACE_FREE(candidates);
        socket_io_instance->last_connect_error = ENOMEM;
        result = __FAILURE__;
    }
    else
    {
        for (candidate_index = 0; candidate_index < candidate_count; candidate_index++)
        {
            attempts[candidate_index].socket = INVALID_SOCKET;
            attempts[candidate_index].address = NULL;
            attempts[candidate_index].state = CONNECTION_ATTEMPT_NOT_STARTED;
            attempts[candidate_index].started_at_ms = 0;
            attempts[candidate_index].candidate_index = candidate_index;
        }

        socket_io_instance->candidates = candidates;
        socket_io_instance->attempts = attempts;
        socket_io_instance->poll_descriptors = poll_descriptors;
        socket_io_instance->polled_attempt_indices = polled_attempt_indices;
        socket_io_instance->candidate_count = candidate_count;
        result = 0;
    }

    return result;
}

static int prepare_connection_candidates(SOCKET_IO_INSTANCE* socket_io_instance, struct addrinfo* address_list)
{
    int result;
    int preferred_family = AF_UNSPEC;
    size_t candidate_count = 0;
    size_t candidate_index;
    struct addrinfo* address;
    struct addrinfo* ipv4_cursor = address_list;
    struct addrinfo* ipv6_cursor = address_list;

    for (address = address_list; address != NULL; address = address->ai_next)
    {
        if (((address->ai_family == AF_INET) || (address->ai_family == AF_INET6)) &&
            (address->ai_addr != NULL))
        {
            if (preferred_family == AF_UNSPEC)
            {
                preferred_family = address->ai_family;
            }
            candidate_count++;
        }
    }

    if (candidate_count == 0)
    {
        LogError("Failure: DNS resolution returned no usable IPv4 or IPv6 addresses.");
        socket_io_instance->last_connect_error = EADDRNOTAVAIL;
        result = __FAILURE__;
    }
    else if (allocate_connection_race_arrays(socket_io_instance, candidate_count) != 0)
    {
        result = __FAILURE__;
    }
    else
    {
        int next_family = preferred_family;

        socket_io_instance->address_list = address_list;

        for (candidate_index = 0; candidate_index < candidate_count; candidate_index++)
        {
            const struct addrinfo* candidate;
            struct addrinfo** cursor = (next_family == AF_INET6) ? &ipv6_cursor : &ipv4_cursor;

            candidate = take_next_candidate(cursor, next_family);
            if (candidate == NULL)
            {
                next_family = (next_family == AF_INET6) ? AF_INET : AF_INET6;
                cursor = (next_family == AF_INET6) ? &ipv6_cursor : &ipv4_cursor;
                candidate = take_next_candidate(cursor, next_family);
            }

            socket_io_instance->candidates[candidate_index] = candidate;
            next_family = (candidate->ai_family == AF_INET6) ? AF_INET : AF_INET6;
        }

        LogInfo("Prepared %zu connection candidates; preserving the operating system's %s preference.",
            candidate_count, (preferred_family == AF_INET6) ? "IPv6" : "IPv4");
        result = 0;
    }

    return result;
}
#endif

/*this function will clone an option given by name and value*/
static void* socketio_CloneOption(const char* name, const void* value)
{
    void* result;

    if (name != NULL)
    {
        result = NULL;

        if (strcmp(name, OPTION_NET_INT_MAC_ADDRESS) == 0)
        {
            if (value == NULL)
            {
                LogError("Failed cloning option %s (value is NULL)", name);
            }
            else
            {
                size_t malloc_size = safe_add_size_t(strlen((char*)value), 1);
                malloc_size = safe_multiply_size_t(malloc_size, sizeof(char));
                if (malloc_size == SIZE_MAX)
                {
                    LogError("Invalid malloc size");
                }
                else if ((result = malloc(malloc_size)) == NULL)
                {
                    LogError("Failed cloning option %s (malloc failed)", name);
                }
                else if (strcpy((char*)result, (char*)value) == NULL)
                {
                    LogError("Failed cloning option %s (strcpy failed)", name);
                    free(result);
                    result = NULL;
                }
            }
        }
        else
        {
            LogError("Cannot clone option %s (not suppported)", name);
        }
    }
    else
    {
        result = NULL;
    }
    return result;
}

/*this function destroys an option previously created*/
static void socketio_DestroyOption(const char* name, const void* value)
{
    if (name != NULL)
    {
        if (strcmp(name, OPTION_NET_INT_MAC_ADDRESS) == 0 && value != NULL)
        {
            free((void*)value);
        }
    }
}

static OPTIONHANDLER_HANDLE socketio_retrieveoptions(CONCRETE_IO_HANDLE handle)
{
    OPTIONHANDLER_HANDLE result;

    if (handle == NULL)
    {
        LogError("failed retrieving options (handle is NULL)");
        result = NULL;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)handle;

        result = OptionHandler_Create(socketio_CloneOption, socketio_DestroyOption, socketio_setoption);
        if (result == NULL)
        {
            LogError("unable to OptionHandler_Create");
        }
        else if (socket_io_instance->target_mac_address != NULL &&
            OptionHandler_AddOption(result, OPTION_NET_INT_MAC_ADDRESS, socket_io_instance->target_mac_address) != OPTIONHANDLER_OK)
        {
            LogError("failed retrieving options (failed adding net_interface_mac_address)");
            OptionHandler_Destroy(result);
            result = NULL;
        }
    }

    return result;
}

static const IO_INTERFACE_DESCRIPTION socket_io_interface_description =
{
    socketio_retrieveoptions,
    socketio_create,
    socketio_destroy,
    socketio_open,
    socketio_close,
    socketio_send,
    socketio_dowork,
    socketio_setoption
};

static void indicate_error(SOCKET_IO_INSTANCE* socket_io_instance)
{
    if (socket_io_instance->on_io_error != NULL)
    {
        socket_io_instance->on_io_error(socket_io_instance->on_io_error_context);
    }
}

static int add_pending_io(SOCKET_IO_INSTANCE* socket_io_instance, const unsigned char* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result;
    PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)malloc(sizeof(PENDING_SOCKET_IO));
    if (pending_socket_io == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        pending_socket_io->bytes = (unsigned char*)malloc(size);
        if (pending_socket_io->bytes == NULL)
        {
            LogError("Allocation Failure: Unable to allocate pending list.");
            free(pending_socket_io);
            result = __FAILURE__;
        }
        else
        {
            pending_socket_io->size = size;
            pending_socket_io->on_send_complete = on_send_complete;
            pending_socket_io->callback_context = callback_context;
            pending_socket_io->pending_io_list = socket_io_instance->pending_io_list;
            (void)memcpy(pending_socket_io->bytes, buffer, size);

            if (singlylinkedlist_add(socket_io_instance->pending_io_list, pending_socket_io) == NULL)
            {
                LogError("Failure: Unable to add socket to pending list.");
                free(pending_socket_io->bytes);
                free(pending_socket_io);
                result = __FAILURE__;
            }
            else
            {
                result = 0;
            }
        }
    }
    return result;
}

static STATIC_VAR_UNUSED void signal_callback(int signum)
{
    AZURE_UNREFERENCED_PARAMETER(signum);
    LogError("Socket received signal %d.", signum);
}

#ifndef __APPLE__
static void destroy_network_interface_descriptions(NETWORK_INTERFACE_DESCRIPTION* nid)
{
    if (nid != NULL)
    {
        if (nid->next != NULL)
        {
            destroy_network_interface_descriptions(nid->next);
        }

        if (nid->name != NULL)
        {
            free(nid->name);
        }

        if (nid->mac_address != NULL)
        {
            free(nid->mac_address);
        }

        if (nid->ip_address != NULL)
        {
            free(nid->ip_address);
        }

        free(nid);
    }
}

static NETWORK_INTERFACE_DESCRIPTION* create_network_interface_description(struct ifreq *ifr, NETWORK_INTERFACE_DESCRIPTION* previous_nid)
{
    NETWORK_INTERFACE_DESCRIPTION* result;
    size_t malloc_size = 0;

    if ((result = (NETWORK_INTERFACE_DESCRIPTION*)malloc(sizeof(NETWORK_INTERFACE_DESCRIPTION))) == NULL)
    {
        LogError("Failed allocating NETWORK_INTERFACE_DESCRIPTION");
    }
    else if ((malloc_size = safe_multiply_size_t(safe_add_size_t(strlen(ifr->ifr_name), 1), sizeof(char))) == SIZE_MAX)
    {
        LogError("invalid malloc size");
        destroy_network_interface_descriptions(result);
        result = NULL;
    }
    else if ((result->name = (char*)malloc(malloc_size)) == NULL)
    {
        LogError("failed setting interface description name (malloc failed)");
        destroy_network_interface_descriptions(result);
        result = NULL;
    }
    else if (strcpy(result->name, ifr->ifr_name) == NULL)
    {
        LogError("failed setting interface description name (strcpy failed)");
        destroy_network_interface_descriptions(result);
        result = NULL;
    }
    else
    {
        char* ip_address;
        unsigned char* mac = (unsigned char*)ifr->ifr_hwaddr.sa_data;

        malloc_size = safe_multiply_size_t(sizeof(char), MAC_ADDRESS_STRING_LENGTH);

        if (malloc_size == SIZE_MAX ||
            (result->mac_address = (char*)malloc(malloc_size)) == NULL)
        {
            LogError("failed formatting mac address (malloc failed) size:%zu", malloc_size);
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if (sprintf(result->mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]) <= 0)
        {
            LogError("failed formatting mac address (sprintf failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if ((ip_address = inet_ntoa(((struct sockaddr_in*)&ifr->ifr_addr)->sin_addr)) == NULL)
        {
            LogError("failed setting the ip address (inet_ntoa failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if ((malloc_size = safe_multiply_size_t(safe_add_size_t(strlen(ip_address), 1), sizeof(char))) == SIZE_MAX)
        {
            LogError("invalid malloc size");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if ((result->ip_address = (char*)malloc(malloc_size)) == NULL)
        {
            LogError("failed setting the ip address (malloc failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if (strcpy(result->ip_address, ip_address) == NULL)
        {
            LogError("failed setting the ip address (strcpy failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else
        {
            result->next = NULL;

            if (previous_nid != NULL)
            {
                previous_nid->next = result;
            }
        }
    }

    return result;
}

static int get_network_interface_descriptions(int socket, NETWORK_INTERFACE_DESCRIPTION** nid)
{
    int result;

    struct ifreq ifr;
    struct ifconf ifc;
    char buf[IFREQ_BUFFER_SIZE];

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;

    if (ioctl(socket, SIOCGIFCONF, &ifc) == -1)
    {
        LogError("ioctl failed querying socket (SIOCGIFCONF, errno=%s)", errno);
        result = __FAILURE__;
    }
    else
    {
        NETWORK_INTERFACE_DESCRIPTION* root_nid = NULL;
        NETWORK_INTERFACE_DESCRIPTION* new_nid = NULL;

        struct ifreq* it = ifc.ifc_req;
        const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

        result = 0;

        for (; it != end; ++it)
        {
            strcpy(ifr.ifr_name, it->ifr_name);

            if (ioctl(socket, SIOCGIFFLAGS, &ifr) != 0)
            {
                LogError("ioctl failed querying socket (SIOCGIFFLAGS, errno=%d)", errno);
                result = __FAILURE__;
                break;
            }
            else if (ioctl(socket, SIOCGIFHWADDR, &ifr) != 0)
            {
                LogError("ioctl failed querying socket (SIOCGIFHWADDR, errno=%d)", errno);
                result = __FAILURE__;
                break;
            }
            else if (ioctl(socket, SIOCGIFADDR, &ifr) != 0)
            {
                LogError("ioctl failed querying socket (SIOCGIFADDR, errno=%d)", errno);
                result = __FAILURE__;
                break;
            }
            else if ((new_nid = create_network_interface_description(&ifr, new_nid)) == NULL)
            {
                LogError("Failed creating network interface description");
                result = __FAILURE__;
                break;
            }
            else if (root_nid == NULL)
            {
                root_nid = new_nid;
            }
        }

        if (result == 0)
        {
            *nid = root_nid;
        }
        else
        {
            destroy_network_interface_descriptions(root_nid);
        }
    }

    return result;
}

static int set_target_network_interface(int target_socket, char* mac_address)
{
    int result;
    int enumeration_socket;
    NETWORK_INTERFACE_DESCRIPTION* nid;

    enumeration_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (enumeration_socket < SOCKET_SUCCESS)
    {
        LogError("Failed creating socket for network interface enumeration (%d)", errno);
        result = __FAILURE__;
    }
    else if (get_network_interface_descriptions(enumeration_socket, &nid) != 0)
    {
        LogError("Failed getting network interface descriptions");
        result = __FAILURE__;
    }
    else
    {
        NETWORK_INTERFACE_DESCRIPTION* current_nid = nid;

        while(current_nid != NULL)
        {
            if (strcmp(mac_address, current_nid->mac_address) == 0)
            {
                break;
            }

            current_nid = current_nid->next;
        }

        if (current_nid == NULL)
        {
            LogError("Did not find a network interface matching MAC ADDRESS");
            result = __FAILURE__;
        }
        else if (setsockopt(target_socket, SOL_SOCKET, SO_BINDTODEVICE, current_nid->name, strlen(current_nid->name)) != 0)
        {
            LogError("setsockopt failed (%d)", errno);
            result = __FAILURE__;
        }
        else
        {
            result = 0;
        }

        destroy_network_interface_descriptions(nid);
    }

    if (enumeration_socket >= SOCKET_SUCCESS)
    {
        close(enumeration_socket);
    }

    return result;
}
#endif //__APPLE__

#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
static void reset_connection_attempt(CONNECTION_ATTEMPT* attempt)
{
    attempt->socket = INVALID_SOCKET;
    attempt->address = NULL;
    attempt->state = CONNECTION_ATTEMPT_NOT_STARTED;
    attempt->started_at_ms = 0;
    attempt->candidate_index = SIZE_MAX;
}

static void fail_connection_attempt(SOCKET_IO_INSTANCE* socket_io_instance, CONNECTION_ATTEMPT* attempt, int error_code)
{
    if (attempt->socket != INVALID_SOCKET)
    {
        SOCKETIO_BERKELEY_CLOSE(attempt->socket);
        socket_io_instance->active_attempt_count--;
    }

    socket_io_instance->last_connect_error = error_code;
    reset_connection_attempt(attempt);
}

static int start_connection_attempt(SOCKET_IO_INSTANCE* socket_io_instance, uint64_t current_time_ms)
{
    int result;
    size_t attempt_index;
    CONNECTION_ATTEMPT* attempt = NULL;
    const struct addrinfo* address;
    int flags;
    int connect_result;
    char resolved_ip[INET6_ADDRSTRLEN] = { 0 };
    const void* resolved_address = NULL;

    for (attempt_index = 0; attempt_index < socket_io_instance->candidate_count; attempt_index++)
    {
        if (socket_io_instance->attempts[attempt_index].state == CONNECTION_ATTEMPT_NOT_STARTED)
        {
            attempt = &socket_io_instance->attempts[attempt_index];
            break;
        }
    }

    if ((attempt == NULL) || (socket_io_instance->next_candidate_index >= socket_io_instance->candidate_count))
    {
        socket_io_instance->last_connect_error = __FAILURE__;
        LogError("Failure: no connection attempt slot or candidate is available.");
        result = __FAILURE__;
    }
    else
    {
        attempt->candidate_index = socket_io_instance->next_candidate_index;
        address = socket_io_instance->candidates[socket_io_instance->next_candidate_index++];
        attempt->address = address;
        attempt->started_at_ms = current_time_ms;
        socket_io_instance->next_attempt_at_ms =
            (current_time_ms > (UINT64_MAX - CONNECTION_ATTEMPT_DELAY_MS)) ?
            UINT64_MAX : current_time_ms + CONNECTION_ATTEMPT_DELAY_MS;
        attempt->socket = SOCKETIO_BERKELEY_SOCKET(address->ai_family, address->ai_socktype, address->ai_protocol);

        if (attempt->socket < SOCKET_SUCCESS)
        {
            int socket_error = (errno != 0) ? errno : EIO;
            LogError("Failure: socket create failure %d (%s).", socket_error, strerror(socket_error));
            fail_connection_attempt(socket_io_instance, attempt, socket_error);
            result = __FAILURE__;
        }
        else
        {
            socket_io_instance->active_attempt_count++;
#ifndef __APPLE__
            errno = 0;
            if ((socket_io_instance->target_mac_address != NULL) &&
                (set_target_network_interface(attempt->socket, socket_io_instance->target_mac_address) != 0))
            {
                int interface_error = (errno != 0) ? errno : ENODEV;
                LogError("Failure: failed selecting target network interface (MACADDR=%s).",
                    socket_io_instance->target_mac_address);
                fail_connection_attempt(socket_io_instance, attempt, interface_error);
                result = __FAILURE__;
            }
            else
#endif //__APPLE__
            if (((flags = SOCKETIO_BERKELEY_FCNTL_GETFL(attempt->socket)) == -1) ||
                (SOCKETIO_BERKELEY_FCNTL_SETFL(attempt->socket, flags | O_NONBLOCK) == -1))
            {
                int fcntl_error = errno;
                LogError("Failure: fcntl failure %d (%s).", fcntl_error, strerror(fcntl_error));
                fail_connection_attempt(socket_io_instance, attempt, fcntl_error);
                result = __FAILURE__;
            }
            else
            {
                if (address->ai_family == AF_INET)
                {
                    resolved_address = &((const struct sockaddr_in*)address->ai_addr)->sin_addr;
                }
                else if (address->ai_family == AF_INET6)
                {
                    resolved_address = &((const struct sockaddr_in6*)address->ai_addr)->sin6_addr;
                }

                if ((resolved_address != NULL) &&
                    (inet_ntop(address->ai_family, resolved_address, resolved_ip, sizeof(resolved_ip)) != NULL))
                {
                    LogInfo("DNS resolved %s to %s, connecting to %s:%d (socket fd=%d)",
                        socket_io_instance->hostname, resolved_ip, socket_io_instance->hostname,
                        socket_io_instance->port, attempt->socket);
                }
                else
                {
                    LogInfo("DNS resolved successfully, connecting to %s:%d (socket fd=%d)",
                        socket_io_instance->hostname, socket_io_instance->port, attempt->socket);
                }

                connect_result = SOCKETIO_BERKELEY_CONNECT(attempt->socket, address->ai_addr, address->ai_addrlen);
                if (connect_result == 0)
                {
                    attempt->state = CONNECTION_ATTEMPT_SUCCEEDED;
                    result = 0;
                }
                else if (errno == EINPROGRESS)
                {
                    attempt->state = CONNECTION_ATTEMPT_CONNECTING;
                    LogInfo("Connect in progress for %s:%d (socket fd=%d).",
                        socket_io_instance->hostname, socket_io_instance->port, attempt->socket);
                    result = 0;
                }
                else
                {
                    int connect_error = errno;
                    LogError("Failure: connect to %s:%d failed with error %d (%s).",
                        socket_io_instance->hostname, socket_io_instance->port,
                        connect_error, strerror(connect_error));
                    fail_connection_attempt(socket_io_instance, attempt, connect_error);
                    result = __FAILURE__;
                }
            }
        }
    }

    return result;
}

static CONNECTION_ATTEMPT* find_successful_connection_attempt(SOCKET_IO_INSTANCE* socket_io_instance)
{
    size_t attempt_index;
    CONNECTION_ATTEMPT* result = NULL;

    for (attempt_index = 0; attempt_index < socket_io_instance->candidate_count; attempt_index++)
    {
        CONNECTION_ATTEMPT* attempt = &socket_io_instance->attempts[attempt_index];
        if ((attempt->state == CONNECTION_ATTEMPT_SUCCEEDED) &&
            ((result == NULL) || (attempt->candidate_index < result->candidate_index)))
        {
            result = attempt;
        }
    }

    return result;
}

static void start_next_connection_attempt(
    SOCKET_IO_INSTANCE* socket_io_instance, uint64_t current_time_ms, int is_first_attempt)
{
    if ((socket_io_instance->next_candidate_index < socket_io_instance->candidate_count) &&
        ((is_first_attempt != 0) || (current_time_ms >= socket_io_instance->next_attempt_at_ms)))
    {
        (void)start_connection_attempt(socket_io_instance, current_time_ms);
    }
}

static void complete_connection_open(SOCKET_IO_INSTANCE* socket_io_instance, CONNECTION_ATTEMPT* winning_attempt)
{
    ON_IO_OPEN_COMPLETE on_io_open_complete = socket_io_instance->on_io_open_complete;
    void* on_io_open_complete_context = socket_io_instance->on_io_open_complete_context;
    IO_OPEN_RESULT_DETAILED open_result_detailed = { IO_OPEN_OK, 0 };
    int connected_socket = winning_attempt->socket;

    winning_attempt->socket = INVALID_SOCKET;
    socket_io_instance->active_attempt_count--;
    socket_io_instance->socket = connected_socket;
    socket_io_instance->on_io_open_complete = NULL;
    socket_io_instance->on_io_open_complete_context = NULL;
    dispose_connection_race(socket_io_instance);
    socket_io_instance->io_state = IO_STATE_OPEN;

    LogInfo("TCP connection to %s:%d established successfully (fd=%d).",
        socket_io_instance->hostname, socket_io_instance->port, connected_socket);

    if (on_io_open_complete != NULL)
    {
        on_io_open_complete(on_io_open_complete_context, open_result_detailed);
    }
}

static void fail_connection_open(SOCKET_IO_INSTANCE* socket_io_instance, int error_code)
{
    ON_IO_OPEN_COMPLETE on_io_open_complete = socket_io_instance->on_io_open_complete;
    void* on_io_open_complete_context = socket_io_instance->on_io_open_complete_context;
    IO_OPEN_RESULT_DETAILED open_result_detailed = { IO_OPEN_ERROR, error_code };

    socket_io_instance->on_io_open_complete = NULL;
    socket_io_instance->on_io_open_complete_context = NULL;
    dispose_connection_race(socket_io_instance);
    socket_io_instance->on_bytes_received = NULL;
    socket_io_instance->on_bytes_received_context = NULL;
    socket_io_instance->on_io_error = NULL;
    socket_io_instance->on_io_error_context = NULL;
    socket_io_instance->io_state = IO_STATE_CLOSED;

    if (on_io_open_complete != NULL)
    {
        on_io_open_complete(on_io_open_complete_context, open_result_detailed);
    }
}

static int progress_connection_open(SOCKET_IO_INSTANCE* socket_io_instance)
{
    int result = 0;
    int time_error;
    uint64_t current_time_ms;
    size_t attempt_index;
    CONNECTION_ATTEMPT* winning_attempt;
    nfds_t poll_descriptor_count = 0;

    winning_attempt = find_successful_connection_attempt(socket_io_instance);
    if (winning_attempt != NULL)
    {
        complete_connection_open(socket_io_instance, winning_attempt);
        result = 1;
    }
    else if (get_monotonic_time_ms(&current_time_ms, &time_error) != 0)
    {
        fail_connection_open(socket_io_instance, time_error);
        result = 1;
    }
    else if (current_time_ms >= socket_io_instance->overall_deadline_ms)
    {
        LogError("Failure: connection to %s:%d timed out.",
            socket_io_instance->hostname, socket_io_instance->port);
        fail_connection_open(socket_io_instance, ETIMEDOUT);
        result = 1;
    }
    else
    {
        int poll_result = 0;

        for (attempt_index = 0; attempt_index < socket_io_instance->candidate_count; attempt_index++)
        {
            CONNECTION_ATTEMPT* attempt = &socket_io_instance->attempts[attempt_index];
            if (attempt->state == CONNECTION_ATTEMPT_CONNECTING)
            {
                socket_io_instance->poll_descriptors[poll_descriptor_count].fd = attempt->socket;
                socket_io_instance->poll_descriptors[poll_descriptor_count].events = POLLOUT;
                socket_io_instance->poll_descriptors[poll_descriptor_count].revents = 0;
                socket_io_instance->polled_attempt_indices[poll_descriptor_count] = attempt_index;
                poll_descriptor_count++;
            }
        }

        if (poll_descriptor_count > 0)
        {
            poll_result = SOCKETIO_BERKELEY_POLL(
                socket_io_instance->poll_descriptors, poll_descriptor_count, 0);
        }

        if ((poll_result < 0) && (errno != EINTR))
        {
            int poll_error = (errno != 0) ? errno : EIO;
            LogError("Failure: poll failure %d (%s).", poll_error, strerror(poll_error));
            fail_connection_open(socket_io_instance, poll_error);
            result = 1;
        }
        else
        {
            for (attempt_index = 0; attempt_index < poll_descriptor_count; attempt_index++)
            {
                CONNECTION_ATTEMPT* attempt =
                    &socket_io_instance->attempts[socket_io_instance->polled_attempt_indices[attempt_index]];

                if (socket_io_instance->poll_descriptors[attempt_index].revents != 0)
                {
                    int socket_error = 0;
                    socklen_t socket_error_length = sizeof(socket_error);

                    if ((socket_io_instance->poll_descriptors[attempt_index].revents & POLLNVAL) != 0)
                    {
                        LogError("Failure: poll reported an invalid connection socket.");
                        fail_connection_attempt(socket_io_instance, attempt, EBADF);
                    }
                    else if (SOCKETIO_BERKELEY_GETSOCKOPT(attempt->socket, SOL_SOCKET, SO_ERROR,
                        &socket_error, &socket_error_length) != 0)
                    {
                        int getsockopt_error = (errno != 0) ? errno : EIO;
                        LogError("Failure: getsockopt failure %d (%s).",
                            getsockopt_error, strerror(getsockopt_error));
                        fail_connection_attempt(socket_io_instance, attempt, getsockopt_error);
                    }
                    else if (socket_error != 0)
                    {
                        LogError("Failure: connect to %s:%d completed with error %d (%s).",
                            socket_io_instance->hostname, socket_io_instance->port,
                            socket_error, strerror(socket_error));
                        fail_connection_attempt(socket_io_instance, attempt, socket_error);
                    }
                    else
                    {
                        attempt->state = CONNECTION_ATTEMPT_SUCCEEDED;
                    }
                }
            }

            winning_attempt = find_successful_connection_attempt(socket_io_instance);
            if (winning_attempt != NULL)
            {
                complete_connection_open(socket_io_instance, winning_attempt);
                result = 1;
            }
            else
            {
                start_next_connection_attempt(socket_io_instance, current_time_ms, 0);
                winning_attempt = find_successful_connection_attempt(socket_io_instance);

                if (winning_attempt != NULL)
                {
                    complete_connection_open(socket_io_instance, winning_attempt);
                    result = 1;
                }
                else if ((socket_io_instance->active_attempt_count == 0) &&
                    (socket_io_instance->next_candidate_index >= socket_io_instance->candidate_count))
                {
                    int connect_error = socket_io_instance->last_connect_error;
                    LogError("Failure: all prepared addresses failed for %s:%d; last error was %d.",
                        socket_io_instance->hostname, socket_io_instance->port, connect_error);
                    fail_connection_open(socket_io_instance, connect_error);
                    result = 1;
                }
            }
        }
    }

    return result;
}
#endif

CONCRETE_IO_HANDLE socketio_create(void* io_create_parameters)
{
    SOCKETIO_CONFIG* socket_io_config = io_create_parameters;
    SOCKET_IO_INSTANCE* result;

    if (socket_io_config == NULL)
    {
        LogError("Invalid argument: socket_io_config is NULL");
        result = NULL;
    }
    else
    {
        result = malloc(sizeof(SOCKET_IO_INSTANCE));
        if (result != NULL)
        {
            result->pending_io_list = singlylinkedlist_create();
            if (result->pending_io_list == NULL)
            {
                LogError("Failure: singlylinkedlist_create unable to create pending list.");
                free(result);
                result = NULL;
            }
            else
            {
                if (socket_io_config->hostname != NULL)
                {
                    size_t malloc_size = safe_add_size_t(strlen(socket_io_config->hostname), 1);
                    if (malloc_size == SIZE_MAX)
                    {
                        LogError("invalid malloc size");
                        result->hostname = NULL;
                    }
                    else
                    {
                        result->hostname = (char*)malloc(malloc_size);
                        if (result->hostname != NULL)
                        {
                            (void)strcpy(result->hostname, socket_io_config->hostname);
                        }
                    }

                    result->socket = INVALID_SOCKET;
                }
                else
                {
                    result->hostname = NULL;
                    result->socket = *((int*)socket_io_config->accepted_socket);
                }

                if ((result->hostname == NULL) && (result->socket == INVALID_SOCKET))
                {
                    LogError("Failure: hostname == NULL and socket is invalid.");
                    singlylinkedlist_destroy(result->pending_io_list);
                    free(result);
                    result = NULL;
                }
                else
                {
                    result->port = socket_io_config->port;
                    result->target_mac_address = NULL;
                    result->on_bytes_received = NULL;
                    result->on_io_error = NULL;
                    result->on_bytes_received_context = NULL;
                    result->on_io_error_context = NULL;
                    result->io_state = IO_STATE_CLOSED;
#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
                    initialize_connection_race(result);
#endif
                }
            }
        }
        else
        {
            LogError("Allocation Failure: SOCKET_IO_INSTANCE");
        }
    }

    return result;
}

void socketio_destroy(CONCRETE_IO_HANDLE socket_io)
{
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        /* we cannot do much if the close fails, so just ignore the result */
        if (socket_io_instance->socket != INVALID_SOCKET)
        {
            close(socket_io_instance->socket);
        }

#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
        dispose_connection_race(socket_io_instance);
#endif

        /* clear allpending IOs */
        LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        while (first_pending_io != NULL)
        {
            PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
            if (pending_socket_io != NULL)
            {
                free(pending_socket_io->bytes);
                free(pending_socket_io);
            }

            (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);
            first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        }

        singlylinkedlist_destroy(socket_io_instance->pending_io_list);
        free(socket_io_instance->hostname);
        free(socket_io_instance->target_mac_address);
        free(socket_io);
    }
}

#ifndef DUAL_STACK_CONNECTION_RACING_ENABLED
static int connect_to_addrinfo(SOCKET_IO_INSTANCE* socket_io_instance, const struct addrinfo* address, int timeout_ms, int* error_code)
{
    int result;
    int connect_result;
    int flags;
    char resolved_ip[INET6_ADDRSTRLEN] = { 0 };
    const void* resolved_address = NULL;

    if ((address == NULL) || (address->ai_addr == NULL))
    {
        *error_code = __FAILURE__;
        LogError("Failure: DNS resolution returned an invalid address.");
        result = __FAILURE__;
    }
    else
    {
        socket_io_instance->socket = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_io_instance->socket < SOCKET_SUCCESS)
        {
            *error_code = errno;
            LogError("Failure: socket create failure %d (%s).", *error_code, strerror(*error_code));
            result = __FAILURE__;
        }
#ifndef __APPLE__
        else if (socket_io_instance->target_mac_address != NULL &&
            set_target_network_interface(socket_io_instance->socket, socket_io_instance->target_mac_address) != 0)
        {
            *error_code = __FAILURE__;
            LogError("Failure: failed selecting target network interface (MACADDR=%s).", socket_io_instance->target_mac_address);
            result = __FAILURE__;
        }
#endif //__APPLE__
        else if ((-1 == (flags = fcntl(socket_io_instance->socket, F_GETFL, 0))) ||
            (fcntl(socket_io_instance->socket, F_SETFL, flags | O_NONBLOCK) == -1))
        {
            *error_code = errno;
            LogError("Failure: fcntl failure %d (%s).", *error_code, strerror(*error_code));
            result = __FAILURE__;
        }
        else
        {
            if (address->ai_family == AF_INET)
            {
                resolved_address = &((const struct sockaddr_in*)address->ai_addr)->sin_addr;
            }
            else if (address->ai_family == AF_INET6)
            {
                resolved_address = &((const struct sockaddr_in6*)address->ai_addr)->sin6_addr;
            }

            if ((resolved_address != NULL) &&
                (inet_ntop(address->ai_family, resolved_address, resolved_ip, sizeof(resolved_ip)) != NULL))
            {
                LogInfo("DNS resolved %s to %s, connecting to %s:%d (socket fd=%d)",
                    socket_io_instance->hostname, resolved_ip, socket_io_instance->hostname,
                    socket_io_instance->port, socket_io_instance->socket);
            }
            else
            {
                LogInfo("DNS resolved successfully, connecting to %s:%d (socket fd=%d)",
                    socket_io_instance->hostname, socket_io_instance->port, socket_io_instance->socket);
            }

            connect_result = connect(socket_io_instance->socket, address->ai_addr, address->ai_addrlen);
            if ((connect_result != 0) && (errno != EINPROGRESS))
            {
                *error_code = errno;
                LogError("Failure: connect to %s:%d failed with error %d (%s).",
                    socket_io_instance->hostname, socket_io_instance->port, *error_code, strerror(*error_code));
                result = __FAILURE__;
            }
            else if (connect_result != 0)
            {
                int poll_result;
                int poll_error = 0;
                struct pollfd fd = { 0 };
                fd.fd = socket_io_instance->socket;
                fd.events = POLLOUT;

                LogInfo("Connect in progress (EINPROGRESS), waiting up to %d milliseconds for %s:%d",
                    timeout_ms, socket_io_instance->hostname, socket_io_instance->port);

                do
                {
                    poll_result = poll(&fd, 1, timeout_ms);
                    if (poll_result < 0)
                    {
                        poll_error = errno;
                    }
                } while ((poll_result < 0) && (poll_error == EINTR));

                if (poll_result == 0)
                {
                    *error_code = SOCKETIO_POLL_TIMEOUT_ERROR;
                    LogError("Failure: connection timed out after %d milliseconds waiting for %s:%d.",
                        timeout_ms, socket_io_instance->hostname, socket_io_instance->port);
                    result = __FAILURE__;
                }
                else if (poll_result < 0)
                {
                    *error_code = poll_error;
                    LogError("Failure: poll failure, retval %d, errno %d (%s).",
                        poll_result, poll_error, strerror(poll_error));
                    result = __FAILURE__;
                }
                else
                {
                    int socket_error = 0;
                    socklen_t socket_error_length = sizeof(socket_error);

                    if (getsockopt(socket_io_instance->socket, SOL_SOCKET, SO_ERROR,
                        &socket_error, &socket_error_length) != 0)
                    {
                        *error_code = errno;
                        LogError("Failure: getsockopt failure %d (%s).", *error_code, strerror(*error_code));
                        result = __FAILURE__;
                    }
                    else if (socket_error != 0)
                    {
                        *error_code = socket_error;
                        LogError("Failure: connect to %s:%d failed with error %d (%s).",
                            socket_io_instance->hostname, socket_io_instance->port,
                            socket_error, strerror(socket_error));
                        result = __FAILURE__;
                    }
                    else
                    {
                        result = 0;
                    }
                }
            }
            else
            {
                result = 0;
            }
        }
    }

    if (result != 0)
    {
        if (socket_io_instance->socket >= SOCKET_SUCCESS)
        {
            close(socket_io_instance->socket);
        }
        socket_io_instance->socket = INVALID_SOCKET;
    }
    else
    {
        *error_code = 0;
        LogInfo("TCP connection to %s:%d established successfully (fd=%d).",
            socket_io_instance->hostname, socket_io_instance->port, socket_io_instance->socket);
    }

    return result;
}
#endif

int socketio_open(CONCRETE_IO_HANDLE socket_io, ON_IO_OPEN_COMPLETE on_io_open_complete, void* on_io_open_complete_context, ON_BYTES_RECEIVED on_bytes_received, void* on_bytes_received_context, ON_IO_ERROR on_io_error, void* on_io_error_context)
{
    int result;
#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
    int open_pending = 0;
#endif

    IO_OPEN_RESULT_DETAILED open_result_detailed = { IO_OPEN_OK, 0 };

    SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
    if (socket_io == NULL)
    {
        LogError("Invalid argument: SOCKET_IO_INSTANCE is NULL");
        result = open_result_detailed.code = __FAILURE__;
    }
    else
    {
        if (socket_io_instance->io_state != IO_STATE_CLOSED)
        {
            LogError("Failure: socket state is not closed.");
            result = open_result_detailed.code = __FAILURE__;
        }
        else if (socket_io_instance->socket != INVALID_SOCKET)
        {
            // Opening an accepted socket
            socket_io_instance->on_bytes_received_context = on_bytes_received_context;
            socket_io_instance->on_bytes_received = on_bytes_received;
            socket_io_instance->on_io_error = on_io_error;
            socket_io_instance->on_io_error_context = on_io_error_context;

            socket_io_instance->io_state = IO_STATE_OPEN;

            result = 0;
        }
        else
        {
            struct addrinfo* addrInfo;
            char portString[16];

            // Validate hostname before DNS resolution
            if (socket_io_instance->hostname == NULL || socket_io_instance->hostname[0] == '\0')
            {
                LogError("Failure: hostname is NULL or empty");
                open_result_detailed.code = __FAILURE__;
                result = __FAILURE__;
            }
            else
            {
                struct addrinfo addrHint = { 0 };
                addrHint.ai_family = AF_UNSPEC;
                addrHint.ai_socktype = SOCK_STREAM;
                addrHint.ai_protocol = 0;

                sprintf(portString, "%u", socket_io_instance->port);
                LogInfo("Starting DNS lookup for %s:%d", socket_io_instance->hostname, socket_io_instance->port);
                int err = SOCKETIO_BERKELEY_GETADDRINFO(socket_io_instance->hostname, portString, &addrHint, &addrInfo);
                if (err != 0)
                {
                    LogError("Failure: getaddrinfo failure %d (%s) for host %s.", err, gai_strerror(err), socket_io_instance->hostname);
                    open_result_detailed.code = err;
                    result = __FAILURE__;
                }
                else
                {
                    int connect_error = __FAILURE__;
#ifndef DUAL_STACK_CONNECTION_RACING_ENABLED
                    size_t address_count = 0;
                    struct addrinfo* address;
#endif

#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
                    if (prepare_connection_candidates(socket_io_instance, addrInfo) != 0)
                    {
                        connect_error = socket_io_instance->last_connect_error;
                        SOCKETIO_BERKELEY_FREEADDRINFO(addrInfo);
                        result = __FAILURE__;
                    }
                    else
                    {
                        uint64_t current_time_ms;
                        int time_error;

                        if (get_monotonic_time_ms(&current_time_ms, &time_error) != 0)
                        {
                            connect_error = time_error;
                            dispose_connection_race(socket_io_instance);
                            result = __FAILURE__;
                        }
                        else if (current_time_ms > (UINT64_MAX - (CONNECT_TIMEOUT_SECONDS * 1000)))
                        {
                            connect_error = EOVERFLOW;
                            LogError("Failure: connection deadline cannot be represented.");
                            dispose_connection_race(socket_io_instance);
                            result = __FAILURE__;
                        }
                        else
                        {
                            socket_io_instance->overall_deadline_ms =
                                current_time_ms + (CONNECT_TIMEOUT_SECONDS * 1000);
                            socket_io_instance->on_io_open_complete = on_io_open_complete;
                            socket_io_instance->on_io_open_complete_context = on_io_open_complete_context;
                            socket_io_instance->on_bytes_received = on_bytes_received;
                            socket_io_instance->on_bytes_received_context = on_bytes_received_context;
                            socket_io_instance->on_io_error = on_io_error;
                            socket_io_instance->on_io_error_context = on_io_error_context;
                            socket_io_instance->io_state = IO_STATE_OPENING;

                            start_next_connection_attempt(socket_io_instance, current_time_ms, 1);
                            open_pending = 1;
                            result = 0;
                        }
                    }
#else
                    for (address = addrInfo; address != NULL; address = address->ai_next)
                    {
                        address_count++;
                    }

                    result = __FAILURE__;
                    for (address = addrInfo; address != NULL; address = address->ai_next)
                    {
                        int timeout_ms = (address_count == 0)
                            ? CONNECT_TIMEOUT_SECONDS * 1000
                            : (CONNECT_TIMEOUT_SECONDS * 1000) / (int)address_count;

                        if (connect_to_addrinfo(socket_io_instance, address, timeout_ms, &connect_error) == 0)
                        {
                            result = 0;
                            break;
                        }
                    }
                    freeaddrinfo(addrInfo);

                    if (result == 0)
                    {
                        socket_io_instance->on_bytes_received = on_bytes_received;
                        socket_io_instance->on_bytes_received_context = on_bytes_received_context;
                        socket_io_instance->on_io_error = on_io_error;
                        socket_io_instance->on_io_error_context = on_io_error_context;
                        socket_io_instance->io_state = IO_STATE_OPEN;
                    }
                    else
                    {
                        open_result_detailed.code = connect_error;
                    }
#endif
#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
                    if (result != 0)
                    {
                        open_result_detailed.code = connect_error;
                    }
#endif
                }
            }
        }
    }

#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
    if (open_pending != 0)
    {
        return 0;
    }
#endif

    if (on_io_open_complete != NULL)
    {
        open_result_detailed.result = result == 0 ? IO_OPEN_OK : IO_OPEN_ERROR;
        on_io_open_complete(on_io_open_complete_context, open_result_detailed);

        /* The xio_open contract is callback-based. Returning a failure code from here
           prevents upstream IO layers from surfacing open_result_detailed.code.
           Always return success once the callback has been invoked. */
        return 0;
    }

    return result;
}

int socketio_close(CONCRETE_IO_HANDLE socket_io, ON_IO_CLOSE_COMPLETE on_io_close_complete, void* callback_context)
{
    int result = 0;

    if (socket_io == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
        dispose_connection_race(socket_io_instance);
        if ((socket_io_instance->io_state != IO_STATE_CLOSED) && (socket_io_instance->io_state != IO_STATE_CLOSING))
        {
            if (socket_io_instance->socket != INVALID_SOCKET)
            {
                LogInfo("Closing socket (fd=%d)", socket_io_instance->socket);
                (void)shutdown(socket_io_instance->socket, SHUT_RDWR);
                close(socket_io_instance->socket);
            }
            socket_io_instance->socket = INVALID_SOCKET;
            socket_io_instance->io_state = IO_STATE_CLOSED;
        }
        socket_io_instance->on_bytes_received = NULL;
        socket_io_instance->on_bytes_received_context = NULL;
        socket_io_instance->on_io_error = NULL;
        socket_io_instance->on_io_error_context = NULL;
#else
        if ((socket_io_instance->io_state != IO_STATE_CLOSED) && (socket_io_instance->io_state != IO_STATE_CLOSING))
        {
            // Only close if the socket isn't already in the closed or closing state
            LogInfo("Closing socket (fd=%d)", socket_io_instance->socket);
            (void)shutdown(socket_io_instance->socket, SHUT_RDWR);
            close(socket_io_instance->socket);
            socket_io_instance->socket = INVALID_SOCKET;
            socket_io_instance->io_state = IO_STATE_CLOSED;
        }
#endif

        if (on_io_close_complete != NULL)
        {
            on_io_close_complete(callback_context);
        }

        result = 0;
    }

    return result;
}

int socketio_send(CONCRETE_IO_HANDLE socket_io, const void* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result;

    if ((socket_io == NULL) ||
        (buffer == NULL) ||
        (size == 0))
    {
        /* Invalid arguments */
        LogError("Invalid argument: send given invalid parameter");
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if (socket_io_instance->io_state != IO_STATE_OPEN)
        {
            LogError("Failure: socket state is not opened.");
            result = __FAILURE__;
        }
        else
        {
            LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
            if (first_pending_io != NULL)
            {
                if (add_pending_io(socket_io_instance, buffer, size, on_send_complete, callback_context) != 0)
                {
                    LogError("Failure: add_pending_io failed.");
                    result = __FAILURE__;
                }
                else
                {
                    result = 0;
                }
            }
            else
            {
                signal(SIGPIPE, SIG_IGN);

                ssize_t send_result = send(socket_io_instance->socket, buffer, size, 0);
                if (send_result != size)
                {
                    if (send_result == INVALID_SOCKET && errno != EAGAIN)
                    {
                        LogError("Failure: sending socket failed. errno=%d (%s).", errno, strerror(errno));
                        result = __FAILURE__;
                    }
                    else
                    {
                        if (send_result == INVALID_SOCKET && errno == EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                        {
                            // put the full message in the queue
                            send_result = 0;
                        }

                        /* queue remaining data */
                        if (add_pending_io(socket_io_instance, buffer + send_result, size - send_result, on_send_complete, callback_context) != 0)
                        {
                            LogError("Failure: add_pending_io failed.");
                            result = __FAILURE__;
                        }
                        else
                        {
                            result = 0;
                        }
                    }
                }
                else
                {
                    if (on_send_complete != NULL)
                    {
                        on_send_complete(callback_context, IO_SEND_OK);
                    }

                    result = 0;
                }
            }
        }
    }

    return result;
}

void socketio_dowork(CONCRETE_IO_HANDLE socket_io)
{
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
#ifdef DUAL_STACK_CONNECTION_RACING_ENABLED
        if (socket_io_instance->io_state == IO_STATE_OPENING)
        {
            (void)progress_connection_open(socket_io_instance);
            return;
        }
#endif
        LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        while (first_pending_io != NULL)
        {
            PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
            if (pending_socket_io == NULL)
            {
                socket_io_instance->io_state = IO_STATE_ERROR;
                indicate_error(socket_io_instance);
                LogError("Failure: retrieving socket from list");
                break;
            }

            signal(SIGPIPE, SIG_IGN);

            ssize_t send_result = send(socket_io_instance->socket, pending_socket_io->bytes, pending_socket_io->size, 0);
            if (send_result != pending_socket_io->size)
            {
                if (send_result == INVALID_SOCKET)
                {
                    if (errno == EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                    {
                        /*do nothing until next dowork */
                        break;
                    }
                    else
                    {
                        free(pending_socket_io->bytes);
                        free(pending_socket_io);
                        (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);

                        LogError("Failure: sending Socket information. errno=%d (%s).", errno, strerror(errno));
                        socket_io_instance->io_state = IO_STATE_ERROR;
                        indicate_error(socket_io_instance);
                    }
                }
                else
                {
                    /* simply wait until next dowork */
                    size_t remaining = pending_socket_io->size - send_result;
                    (void)memmove(pending_socket_io->bytes, pending_socket_io->bytes + send_result, remaining);
                    pending_socket_io->size = remaining;
                    break;
                }
            }
            else
            {
                if (pending_socket_io->on_send_complete != NULL)
                {
                    pending_socket_io->on_send_complete(pending_socket_io->callback_context, IO_SEND_OK);
                }

                free(pending_socket_io->bytes);
                free(pending_socket_io);
                if (singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io) != 0)
                {
                    socket_io_instance->io_state = IO_STATE_ERROR;
                    indicate_error(socket_io_instance);
                    LogError("Failure: unable to remove socket from list");
                }
            }

            first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        }

        if (socket_io_instance->io_state == IO_STATE_OPEN)
        {
            ssize_t received = 0;
            do
            {
                received = recv(socket_io_instance->socket, socket_io_instance->recv_bytes, RECEIVE_BYTES_VALUE, 0);
                if (received > 0)
                {
                    if (socket_io_instance->on_bytes_received != NULL)
                    {
                        /* Explicitly ignoring here the result of the callback */
                        (void)socket_io_instance->on_bytes_received(socket_io_instance->on_bytes_received_context, socket_io_instance->recv_bytes, received);
                    }
                }
                else if (received == 0)
                {
                    // Do not log error here due to this is probably the socket being closed on the other end
                    LogInfo("Socket closed by peer (fd=%d)", socket_io_instance->socket);
                    indicate_error(socket_io_instance);
                }
                else if (received < 0 && errno != EAGAIN)
                {
                    LogError("Socketio_Failure: Receiving data from endpoint: errno=%d (%s).", errno, strerror(errno));
                    indicate_error(socket_io_instance);
                }

            } while (received > 0 && socket_io_instance->io_state == IO_STATE_OPEN);
        }
    }
}

// Edison is missing this from netinet/tcp.h, but this code still works if we manually define it.
#ifndef SOL_TCP
#define SOL_TCP 6
#endif

#ifndef __APPLE__
static void strtoup(char* str)
{
    if (str != NULL)
    {
        while (*str != '\0')
        {
            if (isalpha((int)*str) && islower((int)*str))
            {
                *str = (char)toupper((int)*str);
            }
            str++;
        }
    }
}
#endif // __APPLE__

int socketio_setoption(CONCRETE_IO_HANDLE socket_io, const char* optionName, const void* value)
{
    int result;
    size_t malloc_size = 0;

    if (socket_io == NULL ||
        optionName == NULL ||
        value == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;

        if (strcmp(optionName, "tcp_keepalive") == 0)
        {
            result = setsockopt(socket_io_instance->socket, SOL_SOCKET, SO_KEEPALIVE, value, sizeof(int));
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, "tcp_keepalive_time") == 0)
        {
#ifdef __APPLE__
            result = setsockopt(socket_io_instance->socket, IPPROTO_TCP, TCP_KEEPALIVE, value, sizeof(int));
#else
            result = setsockopt(socket_io_instance->socket, SOL_TCP, TCP_KEEPIDLE, value, sizeof(int));
#endif
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, "tcp_keepalive_interval") == 0)
        {
            result = setsockopt(socket_io_instance->socket, SOL_TCP, TCP_KEEPINTVL, value, sizeof(int));
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, OPTION_NET_INT_MAC_ADDRESS) == 0)
        {
#ifdef __APPLE__
            LogError("option not supported.");
            result = __FAILURE__;
#else
            if (strlen(value) == 0)
            {
                LogError("option value must be a valid mac address");
                result = __FAILURE__;
            }
            else if ((malloc_size = safe_multiply_size_t(sizeof(char), safe_add_size_t(strlen(value), 1))) == SIZE_MAX)
            {
                LogError("Invalid malloc size");
                result = __FAILURE__;
                socket_io_instance->target_mac_address = NULL;
            }
            else if ((socket_io_instance->target_mac_address = (char*)malloc(malloc_size)) == NULL)
            {
                LogError("failed setting net_interface_mac_address option (malloc failed)");
                result = __FAILURE__;
            }
            else if (strcpy(socket_io_instance->target_mac_address, value) == NULL)
            {
                LogError("failed setting net_interface_mac_address option (strcpy failed)");
                free(socket_io_instance->target_mac_address);
                socket_io_instance->target_mac_address = NULL;
                result = __FAILURE__;
            }
            else
            {
                strtoup(socket_io_instance->target_mac_address);
                result = 0;
            }
#endif
        }
        else if (strcmp(optionName, "tcp_nodelay") == 0)
        {
            result = setsockopt(socket_io_instance->socket, IPPROTO_TCP, TCP_NODELAY, value, sizeof(int));
            if (result == -1) result = errno;
        }
        else
        {
            result = __FAILURE__;
        }
    }

    return result;
}

const IO_INTERFACE_DESCRIPTION* socketio_get_interface_description(void)
{
    return &socket_io_interface_description;
}
