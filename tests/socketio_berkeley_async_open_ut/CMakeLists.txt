#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 3.12)

add_executable(socketio_berkeley_async_open_ut_exe
    socketio_berkeley_async_open_ut.c
)

target_include_directories(socketio_berkeley_async_open_ut_exe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../..
    ${CMAKE_CURRENT_LIST_DIR}/../../inc
)

target_link_libraries(socketio_berkeley_async_open_ut_exe aziotsharedutil)

if(use_openssl AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(socketio_berkeley_async_open_ut_exe PRIVATE
        SOCKETIO_BERKELEY_ASYNC_OPEN_TLS_LAYER_TEST
    )
endif()

add_test(
    NAME socketio_berkeley_async_open_ut
    COMMAND socketio_berkeley_async_open_ut_exe
)
