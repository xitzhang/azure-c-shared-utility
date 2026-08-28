# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

if(NOT DEFINED TEST_SOURCE_DIR OR NOT DEFINED TEST_BINARY_ROOT OR NOT DEFINED TEST_GENERATOR)
    message(FATAL_ERROR "Missing CMake validation test arguments")
endif()

file(REMOVE_RECURSE "${TEST_BINARY_ROOT}")
file(MAKE_DIRECTORY
    "${TEST_BINARY_ROOT}/old/include/curl"
    "${TEST_BINARY_ROOT}/new/include/curl"
    "${TEST_BINARY_ROOT}/sysroot/usr/include/curl"
    "${TEST_BINARY_ROOT}/sysroot/usr/lib"
    "${TEST_BINARY_ROOT}/old-build"
    "${TEST_BINARY_ROOT}/new-build"
    "${TEST_BINARY_ROOT}/cross-build")

file(WRITE "${TEST_BINARY_ROOT}/old/include/curl/curl.h" "#include \"curlver.h\"\n")
file(WRITE "${TEST_BINARY_ROOT}/old/include/curl/curlver.h" "#define LIBCURL_VERSION \"7.58.0\"\n")
file(WRITE "${TEST_BINARY_ROOT}/old/libcurl.a" "")

file(WRITE "${TEST_BINARY_ROOT}/new/include/curl/curl.h" "#include \"curlver.h\"\n")
file(WRITE "${TEST_BINARY_ROOT}/new/include/curl/curlver.h" "#define LIBCURL_VERSION \"7.59.0\"\n")
file(WRITE "${TEST_BINARY_ROOT}/new/libcurl.a" "")

file(WRITE "${TEST_BINARY_ROOT}/sysroot/usr/include/curl/curl.h" "#include \"curlver.h\"\n")
file(WRITE "${TEST_BINARY_ROOT}/sysroot/usr/include/curl/curlver.h" "#define LIBCURL_VERSION \"7.59.0\"\n")
file(WRITE "${TEST_BINARY_ROOT}/sysroot/usr/lib/libcurl.a" "")

set(_common_arguments
    -G "${TEST_GENERATOR}"
    -Denable_dual_stack_connection_racing=ON
    -Duse_http=ON
    -Duse_builtin_httpapi=OFF
    -Duse_openssl=OFF
    -Duse_wsio=OFF
    -Dskip_samples=ON)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        ${_common_arguments}
        -DCURL_FOUND=TRUE
        "-DCURL_INCLUDE_DIRS=${TEST_BINARY_ROOT}/old/include"
        "-DCURL_LIBRARIES=${TEST_BINARY_ROOT}/old/libcurl.a"
        "${TEST_SOURCE_DIR}"
    WORKING_DIRECTORY "${TEST_BINARY_ROOT}/old-build"
    RESULT_VARIABLE _old_result
    OUTPUT_VARIABLE _old_output
    ERROR_VARIABLE _old_error)
set(_old_log "${_old_output}${_old_error}")
if(_old_result EQUAL 0)
    message(FATAL_ERROR "Preseeded libcurl 7.58.0 headers unexpectedly passed")
endif()
if(NOT _old_log MATCHES "requires target libcurl" OR NOT _old_log MATCHES "headers >= 7.59.0")
    message(FATAL_ERROR "Old-header failure was not clear:\n${_old_log}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        ${_common_arguments}
        -DCURL_FOUND=TRUE
        "-DCURL_INCLUDE_DIRS=${TEST_BINARY_ROOT}/new/include"
        "-DCURL_LIBRARIES=${TEST_BINARY_ROOT}/new/libcurl.a"
        "${TEST_SOURCE_DIR}"
    WORKING_DIRECTORY "${TEST_BINARY_ROOT}/new-build"
    RESULT_VARIABLE _new_result
    OUTPUT_VARIABLE _new_output
    ERROR_VARIABLE _new_error)
if(NOT _new_result EQUAL 0)
    message(FATAL_ERROR "Preseeded libcurl 7.59.0 headers failed:\n${_new_output}${_new_error}")
endif()

set(_pkg_config_marker "${TEST_BINARY_ROOT}/pkg-config-invoked")
file(WRITE "${TEST_BINARY_ROOT}/failing-pkg-config"
    "#!/bin/sh\nprintf invoked > \"${_pkg_config_marker}\"\nexit 99\n")
execute_process(COMMAND chmod +x "${TEST_BINARY_ROOT}/failing-pkg-config"
    RESULT_VARIABLE _chmod_result)
if(NOT _chmod_result EQUAL 0)
    message(FATAL_ERROR "Could not make fake pkg-config executable")
endif()

file(TO_CMAKE_PATH "${TEST_BINARY_ROOT}/sysroot" _sysroot)
file(TO_CMAKE_PATH "${TEST_BINARY_ROOT}/failing-pkg-config" _pkg_config)
file(WRITE "${TEST_BINARY_ROOT}/cross-toolchain.cmake"
    "set(CMAKE_SYSTEM_NAME Linux)\n"
    "set(CMAKE_FIND_ROOT_PATH \"${_sysroot}\")\n"
    "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
    "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
    "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n"
    "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n"
    "set(PKG_CONFIG_EXECUTABLE \"${_pkg_config}\")\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        ${_common_arguments}
        "-DCMAKE_TOOLCHAIN_FILE=${TEST_BINARY_ROOT}/cross-toolchain.cmake"
        "${TEST_SOURCE_DIR}"
    WORKING_DIRECTORY "${TEST_BINARY_ROOT}/cross-build"
    RESULT_VARIABLE _cross_result
    OUTPUT_VARIABLE _cross_output
    ERROR_VARIABLE _cross_error)
if(NOT _cross_result EQUAL 0)
    message(FATAL_ERROR "Cross sysroot libcurl lookup failed:\n${_cross_output}${_cross_error}")
endif()
if(EXISTS "${_pkg_config_marker}")
    message(FATAL_ERROR "Cross libcurl discovery invoked pkg-config")
endif()

file(READ "${TEST_BINARY_ROOT}/cross-build/CMakeCache.txt" _cross_cache)
if(NOT _cross_cache MATCHES "CURL_INCLUDE_DIR:PATH=${_sysroot}/usr/include")
    message(FATAL_ERROR "Cross lookup did not select the sysroot curl headers")
endif()
if(NOT _cross_cache MATCHES "CURL_LIBRARY:FILEPATH=${_sysroot}/usr/lib/libcurl.a")
    message(FATAL_ERROR "Cross lookup did not select the sysroot curl library")
endif()
