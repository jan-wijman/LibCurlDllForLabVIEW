cmake_minimum_required(VERSION 3.15)
project(curl_lv_wrapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)

set(BUNDLED_LIBCURL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/curl-8.19.0_5-win64-mingw")

if(NOT LIBCURL_ROOT AND EXISTS "${BUNDLED_LIBCURL_ROOT}/include/curl/curl.h")
    set(LIBCURL_ROOT "${BUNDLED_LIBCURL_ROOT}" CACHE PATH "Path to bundled libcurl package")
endif()

if(LIBCURL_ROOT)
    set(LIBCURL_INCLUDE_DIR "${LIBCURL_ROOT}/include" CACHE PATH "Path to libcurl headers")
    set(LIBCURL_LIBRARY "${LIBCURL_ROOT}/lib/libcurl.dll.a" CACHE FILEPATH "Path to libcurl import library")
    if(EXISTS "${LIBCURL_ROOT}/bin/libcurl-x64.dll")
        set(LIBCURL_DLL "${LIBCURL_ROOT}/bin/libcurl-x64.dll" CACHE FILEPATH "Path to libcurl runtime DLL")
    else()
        set(LIBCURL_DLL "${LIBCURL_ROOT}/bin/libcurl.dll" CACHE FILEPATH "Path to libcurl runtime DLL")
    endif()
endif()

if(NOT LIBCURL_INCLUDE_DIR)
    message(FATAL_ERROR "LIBCURL_INCLUDE_DIR must be set to the libcurl include folder for MinGW-w64 builds.")
endif()

if(NOT LIBCURL_LIBRARY)
    message(FATAL_ERROR "LIBCURL_LIBRARY must be set to the libcurl import library for MinGW-w64 builds.")
endif()

find_path(LIBCURL_INCLUDE_DIR_FOUND curl/curl.h HINTS ${LIBCURL_INCLUDE_DIR})
if(NOT LIBCURL_INCLUDE_DIR_FOUND)
    message(FATAL_ERROR "Could not find curl/curl.h in ${LIBCURL_INCLUDE_DIR}")
endif()

add_library(curl_lv SHARED
    include/curl_lv.h
    src/curl_lv.cpp
    src/curl_lv_async.cpp
)

target_include_directories(curl_lv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBCURL_INCLUDE_DIR}
)

target_link_libraries(curl_lv PRIVATE ${LIBCURL_LIBRARY})

set_target_properties(curl_lv PROPERTIES
    OUTPUT_NAME "curl_lv"
    PREFIX ""
)

if(LIBCURL_DLL)
    add_custom_command(TARGET curl_lv POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCURL_DLL}" "$<TARGET_FILE_DIR:curl_lv>"
        COMMENT "Copying libcurl runtime DLL next to curl_lv.dll"
    )
endif()
