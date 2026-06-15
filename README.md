# Muses

A header-only **C++23 collection of thread-safe, high-performance primitives** —
concurrency building blocks (queues, caches, pools, a Bloom filter) and a
networking stack (reactor + poller + HTTP handler). The library is the
collection of `muses/*.hpp` headers; an HTTP server built on top of it lives in
`examples/` as a demonstration of how the primitives compose.

This is a learning/portfolio project. Every component is independently tested
under AddressSanitizer + UBSan. C++23 facilities are used where they help:
`std::format`, `std::flat_map`, `std::expected`, `std::span`, `std::counting_semaphore`.

## Components

General-purpose primitives (`muses/`):

| Component            | Header                  | Notes                                                        |
|----------------------|-------------------------|--------------------------------------------------------------|
| Thread pool          | `thread_pool.hpp`       | fixed-size, futures, `wait_empty()`                          |
| Bounded queue        | `bounded_queue.hpp`     | lock-free Vyukov MPMC ring (DropNew/DropOldest); mutex+deque for Block |
| Unbounded queue      | `queue.hpp`             | blocking push/pop, retained for general use                  |
| Memory pool          | `memory_pool.hpp`       | size-class free-lists + per-alloc refcount + `PoolPtr<T>` + `PooledBuffer` |
| LRU cache            | `lru_cache.hpp`         | O(1) get/put/erase, mutex-locked, entry-count bounded        |
| Counting Bloom filter| `bloom_filter.hpp`      | removable, decay-based expiry, mutex-locked                  |
| Logger               | `logging.hpp`           | async, single background thread, bounded DropOldest ring     |
| Profiler             | `profiler.hpp`          | RAII scope timer (micro-benchmark tool)                      |

Networking stack (`muses/net/`):

| Component            | Header                  | Notes                                                        |
|----------------------|-------------------------|--------------------------------------------------------------|
| Poller               | `net/poller.hpp` + `*_poller.hpp` | edge-triggered kqueue/epoll abstraction + `wakeup()` |
| Reactor              | `net/reactor.hpp`       | sharded (SO_REUSEPORT) reactor pool + async writes + `ReactorPool` |
| HTTP handler         | `net_components/http_handler.hpp` | static files, CRLF, keep-alive, traversal-safe, LRU-cached |

## Build

Requires CMake >= 3.18 and a C++23 compiler (clang >= 17 / gcc >= 14; libc++ or
libstdc++ with `std::flat_map`, `std::expected`, `std::format`, `std::span`).

```bash
cmake -S . -B build
cmake --build build -j
```

Debug builds enable AddressSanitizer + UndefinedBehaviorSanitizer by default.
Disable with `-DMUSES_ENABLE_SANITIZERS=OFF`.

## Tests

Tests use [doctest](https://github.com/doctest/doctest), fetched via
`FetchContent` (no manual install). Each `tests/test_*.cpp` is a standalone
executable registered with CTest. 11 suites, all green under ASan/UBSan.

```bash
cd build && ctest --output-on-failure
```

The reactor and poller tests bind real sockets on `127.0.0.1` ephemeral ports.

## Example: static-file HTTP server

A demo built from the primitives — `ReactorPool` (multi-reactor) +
`HttpContext` (static files) + `ThreadPool` (workers). It is **an example of
using the library, not the library's purpose**; the components above are.

```bash
cd build && ./muses_http_server
# serves ./statics on http://127.0.0.1:8864/
#   MUSES_REACTORS  reactor shards (default: hardware_concurrency())
#   MUSES_WORKERS   worker threads per shard (default 4)
#   MUSES_PORT      listen port (default 8864)
```

```cpp
#include "muses/net/reactor.hpp"
#include "muses/net_components/http_handler.hpp"

muses::TCPListener listener("127.0.0.1", port);
int lfd = *listener.get_listener();           // SO_REUSEPORT enabled

muses::ReactorPool pool(lfd,
    [](const std::string& req) -> muses::HandlerResult {
        auto hr = muses::HttpContext::handle_request(req);
        return {std::move(hr.response), hr.keep_alive};
    }, /*shards=*/4, /*workers_per_shard=*/2);
pool.start();
```

## Architecture notes

**Bounded queue** (`bounded_queue.hpp`): DropNew/DropOldest use a lock-free
Vyukov MPMC bounded ring — per-slot atomic sequence counters, cache-line-padded
producer/consumer cursors, `counting_semaphore` for blocking consumers (decoupled
from the data path, so `try_push`/`try_pop` stay lock-free). Block retains the
proven mutex+deque implementation. This is the logging backend and the reactor
outbox.

**Memory pool** (`memory_pool.hpp`): every allocation is prefixed with a
`BlockHeader` carrying a magic cookie, the owning size-class index, and an
atomic reference count. Allocations are served from per-class free-lists backed
by a pre-allocated arena; oversized requests fall back to `::operator new` and
are tagged `ESCAPED`. `PoolPtr<T>` is the RAII handle (copy = retain, move =
transfer, destroy = release). `PooledBuffer` is a growable byte buffer backed by
the pool, reused across keep-alive requests on a connection.

**LRU cache** (`lru_cache.hpp`): a doubly-linked list (`std::list`) ordered
MRU→LRU paired with an `unordered_map` from key to list iterator. Both splice
and iterator stability are O(1), so get/put/erase are O(1). Mutex-locked for
open-box thread safety.

**Counting Bloom filter** (`bloom_filter.hpp`): 4-bit packed counters support
removal (unlike a standard Bloom) and decay-based expiry (right-shift all
counters periodically) instead of per-entry TTLs — matching rate-limiter /
blacklist semantics without a hashmap. Double-hashing derives `k` hashes from
two base hashes.

**Logger** (`logging.hpp`): producers push entries into a bounded MPMC ring
(DropOldest, so the hot path is never blocked). A single background thread
drains in batches and writes to file. `wait_pop(timeout)` + `stop()` in the
destructor guarantee no deadlock on shutdown.

**Reactor** (`net/reactor.hpp`): a sharded reactor pool — N independent reactor
threads share one listen fd via SO_REUSEPORT (the kernel hands each connection
to one shard, which owns the fd for its lifetime). Each shard runs an
edge-triggered poller (kqueue `EV_CLEAR` / epoll `EPOLLET`), accepts, reads, and
dispatches complete requests to a private worker pool. Workers compute the
response and post it back through a bounded lock-free outbox; the reactor does
the non-blocking write, parking partial writes on the connection and arming
writable interest to drain later. Interest changes are batched into a single
changelist per loop iteration. Handback routing needs no cross-shard locking
because each fd lives on exactly one shard.

**Poller** (`net/poller.hpp`): `EventMask` bit flags + `PollEvent{fd,mask,userdata}`
abstract edge-triggered kqueue/epoll. `wakeup()` (EVFILT_USER on macOS, eventfd
on Linux) lets another thread interrupt a blocked `wait()`.

## Known limitations

- HTTP/1.1 only; no HTTPS, WebSocket, HTTP/2, routing, or middleware.
- Static-file serving only (a reverse-proxy example is a planned follow-up).
- DoS hardening: idle-timeout (slowloris defense), max-connections cap, and
  per-IP connection-rate limiting (sliding window + `CountingBloomFilter`
  blacklist) are implemented and configurable via env vars (`MUSES_IDLE_TIMEOUT`,
  `MUSES_MAX_CONNECTIONS`, `MUSES_IP_RATE`; all default to safe/off). There is
  no request-body rate limiting or authentication.
- Edge-triggered I/O means the read path must drain to EAGAIN and re-check
  buffered pipelined bytes after each response — handled, but it is the kind of
  code that is easy to regress; the pipelined-request test guards it.
- CI was removed (GitHub runners lack a new-enough C++23 toolchain); tested
  locally on Apple clang / libc++ (Darwin 25). epoll code is preserved and
  Linux-correct but not currently exercised in CI.
- Windows / IOCP is not supported.

## License

MIT.
