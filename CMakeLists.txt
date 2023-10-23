cmake_minimum_required(VERSION 3.15)
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

add_executable(muses_test_net_driver ${MUSES_TEST_NET_DRIVER})
add_executable(muses_test_net_driver_client ${MUSES_TEST_NET_DRIVER_CLIENT})