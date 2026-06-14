# Muses

A header-only C++23 collection of concurrency and networking primitives:
a thread pool, a size-classed memory pool with per-allocation reference
counting, a bounded MPMC queue, an asynchronous logger, and a reactor-based
HTTP server (kqueue on macOS, epoll on Linux).

This is a learning/portfolio project. The components are independently tested
under AddressSanitizer + UBSan. They use C++23 facilities where they help:
`std::format` (logging/HTTP formatting), `std::flat_map` (connection tables),
`std::expected` (error paths), `std::span` (poller interface).

## Status

| Component        | Header                                   | Notes                                            |
|------------------|------------------------------------------|--------------------------------------------------|
| Thread pool      | `muses/thread_pool.hpp`                  | fixed-size, futures, `wait_empty()`              |
| Memory pool      | `muses/memory_pool.hpp`                  | size-class free-lists + per-alloc refcount + `PoolPtr<T>` |
| Bounded queue    | `muses/bounded_queue.hpp`                | MPMC, DropNew / DropOldest / Block               |
| (unbounded queue)| `muses/queue.hpp`                        | blocking push/pop, retained for general use      |
| Logger           | `muses/logging.hpp`                      | async, single background thread, DropOldest ring |
| Profiler         | `muses/profiler.hpp`                     | RAII scope timer                                 |
| Poller           | `muses/net/poller.hpp` + `*_poller.hpp`  | kqueue/epoll abstraction + wakeup                |
| Reactor          | `muses/net/reactor.hpp`                  | single I/O thread + blocking worker pool          |
| HTTP handler     | `muses/net_components/http_handler.hpp`  | static files, CRLF, keep-alive, traversal-safe   |

## Build

Requires CMake >= 3.18 and a C++23 compiler (clang >= 17 / gcc >= 14; libc++ or libstdc++ with std::flat_map, std::expected, std::format, std::span).
Note: std::move_only_function and std::stacktrace are NOT yet used (incomplete in current libc++/libstdc++).

```bash
cmake -S . -B build
cmake --build build -j
```

Debug builds enable AddressSanitizer + UndefinedBehaviorSanitizer by default.
Disable with `-DMUSES_ENABLE_SANITIZERS=OFF`.

## Tests

Tests use [doctest](https://github.com/doctest/doctest), fetched via
`FetchContent` (no manual install). Each `tests/test_*.cpp` is a standalone
executable registered with CTest.

```bash
cd build && ctest --output-on-failure
```

The reactor and poller tests bind real sockets on `127.0.0.1` ephemeral ports.

## Example server

```bash
cd build && ./muses_http_server
# serves ./statics on http://127.0.0.1:8864/
```

```cpp
#include "muses/net/reactor.hpp"
#include "muses/net_components/http_handler.hpp"

muses::TCPListener listener("127.0.0.1", 8864);
muses::Reactor reactor(listener.get_listener(),
    [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return {std::move(hr.response), hr.keep_alive};
    }, /*worker_threads=*/4);
reactor.start();
```

## Architecture notes

**Memory pool** (`memory_pool.hpp`): every allocation is prefixed with a
`BlockHeader` carrying a magic cookie, the owning size-class index, and an
atomic reference count. Allocations are served from per-class free-lists
backed by a pre-allocated arena; oversized requests fall back to
`::operator new` and are tagged `ESCAPED` so `release()` routes them back via
`::operator delete`. `PoolPtr<T>` is the RAII handle (copy = retain, move =
transfer, destroy = release). This replaces the old bump-allocator whose
per-block "refcount" was actually an allocation counter and recycled blocks
while still referenced.

**Logger** (`logging.hpp`): producers push entries into a bounded MPMC ring
(`DropOldest`, so the hot path is never blocked). A single background thread
drains in batches and writes to file. The consumer uses `wait_pop(timeout)`
and the destructor calls `stop()` — this fixes the old logger, which deadlocked
because its consumer called `wait_and_pop()` in a loop that blocked forever
once the queue emptied.

**Reactor** (`net/reactor.hpp`): one dedicated I/O thread runs the poller,
accepts connections, and reads requests. A complete request is handed to a
`ThreadPool` worker (the fd is detached from the poller while in flight), the
worker computes the response and does the blocking write, then hands back
`{fd, keep_alive, ok}` through a bounded outbox — the *only* shared mutable
channel between threads. The reactor drains the outbox and either re-arms the
fd (keep-alive) or closes it. Connection state lives in a reactor-thread-only
map, eliminating the old `net_driver`'s data races on `context_map` /
`occuping_fds`.

**Poller** (`net/poller.hpp`): `EventMask` bit flags + `PollEvent{fd,mask,userdata}`
abstract kqueue/epoll. `wakeup()` (EVFILT_USER on macOS, eventfd on Linux) lets
another thread interrupt a blocked `wait()`.

## Known limitations

- HTTP/1.1 only; no HTTPS, WebSocket, HTTP/2, routing, or middleware.
- No pipelined-request handling: a second request received in the same read as
  the first is dropped (the reactor dispatches on the first `\r\n\r\n`).
- `write_all` busy-spins on EAGAIN, which is fine for small responses but would
  block a worker on a slow client with a full socket buffer; large responses
  should register writable interest instead (not yet implemented).
- Logger is a process-wide singleton; the log filename/level are compile-time
  macros.
- Windows / IOCP is not supported.

## License

MIT.
