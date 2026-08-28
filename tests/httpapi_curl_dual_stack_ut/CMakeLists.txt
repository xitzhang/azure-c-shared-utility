# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 3.12)

add_executable(httpapi_curl_dual_stack_ut_exe
    httpapi_curl_dual_stack_ut.c
)

target_include_directories(httpapi_curl_dual_stack_ut_exe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../..
    ${CMAKE_CURRENT_LIST_DIR}/../../inc
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(httpapi_curl_dual_stack_ut_exe aziotsharedutil)

add_test(
    NAME httpapi_curl_dual_stack_ut
    COMMAND httpapi_curl_dual_stack_ut_exe
)

add_test(
    NAME httpapi_curl_dual_stack_cmake_validation
    COMMAND ${CMAKE_COMMAND}
        -DTEST_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/../..
        -DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cmake_validation
        -DTEST_GENERATOR=${CMAKE_GENERATOR}
        -P ${CMAKE_CURRENT_LIST_DIR}/httpapi_curl_dual_stack_cmake_validation.cmake
)
