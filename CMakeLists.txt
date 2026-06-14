cmake_minimum_required(VERSION 3.18)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(muses VERSION 0.1.0 LANGUAGES CXX)

# --- Build configuration ---------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Header-only library: consumers need the include root.
add_library(muses INTERFACE)
target_include_directories(muses INTERFACE ${CMAKE_SOURCE_DIR})
target_compile_features(muses INTERFACE cxx_std_23)

# Warnings apply to everything that links muses (including the tests).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(muses INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
endif()

# ASan + UBSan by default in Debug. Disable with -DMUSES_ENABLE_SANITIZERS=OFF.
option(MUSES_ENABLE_SANITIZERS "Enable AddressSanitizer + UBSan in Debug" ON)
if(MUSES_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug"
       AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(muses INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(muses INTERFACE   -fsanitize=address,undefined)
endif()

# --- doctest (header-only, pulled via FetchContent) -----------------------

include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.12
)
# We use doctest purely as a header. Disable its own build targets.
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

# --- Tests ----------------------------------------------------------------
# One executable per test_*.cpp, each providing its own doctest main via
# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN in tests/main.cpp being linked only into
# the aggregate target. For per-file executables we instead let main.cpp drive
# them: simpler approach — one binary per source, main comes from that same
# TU defining DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.

enable_testing()

file(GLOB MUSES_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)

foreach(src ${MUSES_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE muses)
    # doctest header path (FetchContent populates the include dir).
    target_include_directories(${name} PRIVATE ${doctest_SOURCE_DIR}/doctest)
    # Each test TU defines its own main via DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
    target_compile_definitions(${name} PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# Static assets needed by http handler tests / demo server.
file(COPY ${CMAKE_SOURCE_DIR}/tests/statics DESTINATION ${CMAKE_BINARY_DIR})

# --- Example: static-file HTTP server ------------------------------------
add_executable(muses_http_server ${CMAKE_SOURCE_DIR}/examples/http_server.cpp)
target_link_libraries(muses_http_server PRIVATE muses)
