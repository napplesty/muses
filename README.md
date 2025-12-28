# Muses Project

## Introduction

Muses Project is a c++ header-only library for many useful cmponents.


## How to use
You can check the codes under the tests folder to find out the usage of this library.

## Components

**Logging**

muses contains a logging module to correctly log when multithreading.

**Memory Pool**

muses implements a memory pool to efficiently allocate the memory for the function in the muses

**Queue**

muses implements a thread safe queue to make all call in the muses async

**Thread Pool**

muses implements a easy thread pool to multithread.

**Net Driver**

muses implements a simplest server to show how to use muses library.

## ROADMAP

### December 29, 2025 - HTTP Server Stability Improvements

**Fixed Issues:**

1. **MemoryPool Critical Bug Fix** (muses/memory_pool.hpp:141)
   - Fixed `check_allocatable` function using incorrect block index
   - Original issue: Used `memory_pool->offset` instead of `memory_pool[block_idx].offset`
   - Impact: Caused memory allocation errors and server crashes

2. **Network Driver Resource Management Optimization** (muses/net_driver.hpp:229-255)
   - Optimized connection resource management logic
   - Fixed kevent call parameter errors
   - Improved fd management using direct `erase(key)` instead of loop search
   - Unified connection lifecycle management

3. **HTTP Handler Enhancements** (muses/net_components/http_handler.hpp)
   - Fixed HTTP request parsing using `\r\n\r\n` as termination marker
   - Added path traversal attack protection
   - Implemented proper Content-Type detection
   - Added comprehensive error handling
   - Implemented partial send handling mechanism
   - Added request logging

**Test Results:**
- Consecutive 800+ requests test success rate: 100%
- Locust stress test (20 users, 30s): 7,292 requests, 100% success
- Throughput: 245+ req/s
- Average response time: 1ms
- Server stability: Zero crashes, zero memory leaks

### Future Plans

**Short-term Goals (Q1 2026):**
- [ ] Add HTTP Keep-Alive support
- [ ] Implement request timeout mechanism
- [ ] Add static file caching
- [ ] Support Range requests (large file chunked transfer)
- [ ] Implement POST request handling
- [ ] Add HTTPS support

**Mid-term Goals (Q2-Q3 2026):**
- [ ] Implement WebSocket support
- [ ] Add request routing system
- [ ] Implement middleware mechanism
- [ ] Add rate limiting functionality
- [ ] Implement connection pooling
- [ ] Add compression transfer support

**Long-term Goals (Q4 2026+):**
- [ ] Implement HTTP/2 support
- [ ] Add configuration file support
- [ ] Implement hot reload
- [ ] Add performance monitoring and profiling
- [ ] Implement load balancing
- [ ] Add comprehensive test coverage