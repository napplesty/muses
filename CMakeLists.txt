cmake_minimum_required(VERSION 3.18)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(muses VERSION 0.0.1 LANGUAGES CXX)
 
set(CMAKE_BUILD_TYPE DEBUG)
set(CMAKE_CXX_FLAGS 
    -std=c++20
)

set ( MUSES_HEADERS
    muses/memory_pool.hpp
    muses/thread_pool.hpp
    muses/queue.hpp
    muses/logging.hpp
    muses/compiler_defs.hpp
    muses/profiler.hpp
    muses/net_components/http_handler.hpp
)

set ( MUSES_TEST_FUNCTION
    tests/test_components.cpp
)

include_directories(${CMAKE_SOURCE_DIR})

add_executable(muses_test_function ${MUSES_TEST_FUNCTION})

set ( MUSES_TEST_NET_DRIVER
    tests/test_net_driver.cpp
)

set ( MUSES_TEST_NET_DRIVER_CLIENT
    tests/test_net_driver_client.cpp
)

set ( MUSES_TEST_HTTP_SERVER
    tests/test_http_server.cpp
)


add_executable(muses_test_net_driver ${MUSES_TEST_NET_DRIVER})
add_executable(muses_test_net_driver_client ${MUSES_TEST_NET_DRIVER_CLIENT})

file(COPY ${CMAKE_SOURCE_DIR}/tests/statics DESTINATION ${CMAKE_BINARY_DIR})
add_executable(muses_test_http_server ${MUSES_TEST_HTTP_SERVER})