// MIT License

// Copyright (c) 2023 nastyapple

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "muses/config.hpp"

#ifndef MUSES_MEMORY_POOL_HPP
#define MUSES_MEMORY_POOL_HPP

namespace muses {

// A size-classed memory pool with per-allocation reference counting.
//
// Each allocation is prefixed with a BlockHeader carrying:
//   - a magic cookie to catch wild/double frees,
//   - the owning size-class index (or ESCAPED for oversized allocations),
//   - the slot size of that class,
//   - an atomic reference count.
//
// Allocations are served from free-lists (one per size class) backed by a
// pre-allocated arena. Oversized requests and arena underflow fall back to
// ::operator new and are tagged ESCAPED so release() routes them back to
// ::operator delete instead of corrupting a free-list.
//
// This replaces the old bump-allocator which mis-used a per-block "refcount"
// that was actually an allocation counter, causing blocks to be recycled
// while still referenced.

class MemoryPool {
public:
    static constexpr uint32_t MAGIC = 0x4D55'5345;   // "MUSE"
    static constexpr uint32_t ESCAPED = 0xFFFFu;

    struct alignas(std::max_align_t) BlockHeader {
        uint32_t magic;
        uint32_t size_class;   // index into classes_, or ESCAPED
        uint32_t slot_size;    // usable bytes (user request, not incl header)
        uint32_t padding;      // keep refcount 8-byte aligned
        std::atomic<int32_t> refcount;
    };

    struct Stats {
        std::size_t live;             // allocations currently handed out
        std::size_t recycled;         // currently sitting in free-lists
        std::size_t escaped;          // served via ::operator new
        std::size_t overflow_dropped; // reserved for future policy
    };

    // Default size classes (usable bytes per slot). Sorted ascendingly.
    static constexpr std::initializer_list<uint32_t> DEFAULT_CLASSES = {
        16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    };

    explicit MemoryPool(std::size_t arena_bytes = 1 << 20,
                        std::initializer_list<uint32_t> classes = DEFAULT_CLASSES)
    : arena_bytes_(arena_bytes) {
        // Normalize and sort the size classes.
        for (uint32_t c : classes) {
            if (c > 0) classes_.push_back(c);
        }
        std::sort(classes_.begin(), classes_.end());
        classes_.erase(std::unique(classes_.begin(), classes_.end()), classes_.end());
        if (classes_.empty()) {
            classes_.push_back(64);
        }
        bins_.reserve(classes_.size());
        for (std::size_t i = 0; i < classes_.size(); ++i) {
            bins_.push_back(std::make_unique<Bin>());
        }
        allocate_arena();
    }

    ~MemoryPool() {
        // Arena memory is owned by the pool; escaped allocations are owned by
        // their PoolPtrs and freed via release(). Nothing else to do here.
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    // Allocate `bytes` usable bytes. Returns a pointer to the usable region
    // (header sits immediately before it). refcount is initialized to 1.
    void* allocate(std::size_t bytes) {
        std::size_t need = round_up_to_class(bytes);
        std::size_t idx = pick_class(bytes);
        if (idx >= classes_.size() || need == 0) {
            // Oversized: escape to the global allocator.
            return allocate_escaped(bytes);
        }
        Bin& bin = *bins_[idx];
        std::lock_guard<std::mutex> lock(bin.mutex);
        void* slot = pop_free(bin);
        if (slot == nullptr) {
            slot = carve_from_arena(classes_[idx]);
            if (slot == nullptr) {
                // Arena exhausted for this class; escape.
                return allocate_escaped(bytes);
            }
        }
        BlockHeader* h = header_of(slot);
        init_header(h, static_cast<uint32_t>(idx), classes_[idx], bytes);
        stats_.live.fetch_add(1, std::memory_order_relaxed);
        return slot;
    }

    // Increment the reference count of a live allocation.
    void retain(void* p) {
        if (p == nullptr) return;
        BlockHeader* h = header_of(p);
        check_magic(h);
        h->refcount.fetch_add(1, std::memory_order_relaxed);
    }

    // Decrement the reference count; when it reaches zero the memory is
    // returned to its free-list (or ::operator delete for escaped blocks).
    void release(void* p) {
        if (p == nullptr) return;
        BlockHeader* h = header_of(p);
        check_magic(h);
        if (h->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Last reference: recycle.
            stats_.live.fetch_sub(1, std::memory_order_relaxed);
            if (h->size_class == ESCAPED) {
                stats_.escaped.fetch_sub(1, std::memory_order_relaxed);
                operator delete(h);
            } else {
                recycle_to_bin(h);
            }
        }
    }

    // Alias for release(), kept for source compatibility with old callers.
    void deallocate(void* p) { release(p); }

    Stats stats() const {
        Stats s{};
        s.live = stats_.live.load(std::memory_order_relaxed);
        s.escaped = stats_.escaped.load(std::memory_order_relaxed);
        std::size_t recycled = 0;
        for (const auto& bin_ptr : bins_) {
            std::lock_guard<std::mutex> lock(bin_ptr->mutex);
            recycled += bin_ptr->free_count;
        }
        s.recycled = recycled;
        s.overflow_dropped = stats_.overflow_dropped.load(std::memory_order_relaxed);
        return s;
    }

private:
    struct Bin {
        std::mutex mutex;
        void* head = nullptr;   // intrusive singly-linked free-list
        std::size_t free_count = 0;
    };

    struct AtomicStats {
        std::atomic<std::size_t> live{0};
        std::atomic<std::size_t> escaped{0};
        std::atomic<std::size_t> overflow_dropped{0};
    };

    static_assert(sizeof(BlockHeader) % alignof(std::max_align_t) == 0,
                  "BlockHeader must keep user data max-aligned");

    static BlockHeader* header_of(void* user) {
        return reinterpret_cast<BlockHeader*>(
            static_cast<std::byte*>(user) - sizeof(BlockHeader));
    }

    static void init_header(BlockHeader* h, uint32_t idx, uint32_t slot_size, std::size_t user_bytes) {
        h->magic = MAGIC;
        h->size_class = idx;
        h->slot_size = slot_size;
        h->padding = 0;
        h->refcount.store(1, std::memory_order_relaxed);
        (void)user_bytes;
    }

    static void check_magic(const BlockHeader* h) {
        MUSES_ASSERT(h->magic == MAGIC, "memory pool: bad magic (wild/double free)");
    }

    // Next-link lives in the first word of a free slot's usable region.
    static void** link_ptr(void* slot) {
        return reinterpret_cast<void**>(slot);
    }

    static void push_free(Bin& bin, void* slot) {
        *link_ptr(slot) = bin.head;
        bin.head = slot;
        ++bin.free_count;
    }

    static void* pop_free(Bin& bin) {
        if (bin.head == nullptr) return nullptr;
        void* slot = bin.head;
        bin.head = *link_ptr(slot);
        --bin.free_count;
        return slot;
    }

    std::size_t pick_class(std::size_t user_bytes) const {
        for (std::size_t i = 0; i < classes_.size(); ++i) {
            if (classes_[i] >= user_bytes) return i;
        }
        return classes_.size();  // out of range → escape
    }

    std::size_t round_up_to_class(std::size_t user_bytes) const {
        for (std::size_t i = 0; i < classes_.size(); ++i) {
            if (classes_[i] >= user_bytes) return classes_[i];
        }
        return 0;  // oversized
    }

    void allocate_arena() {
        std::size_t unit = sizeof(BlockHeader) + (classes_.empty() ? 64 : classes_.back());
        unit = round_up(unit, alignof(std::max_align_t));
        std::size_t count = arena_bytes_ / unit;
        if (count == 0) count = 1;
        arena_bytes_ = count * unit;
        arena_.resize(arena_bytes_, std::byte{0});
        bump_ = 0;
    }

    // Carve a fresh [header+slot] from the arena bump pointer. Not locked
    // here; callers arrange locking as needed (called under the target bin's
    // lock when servicing allocate, or under no lock during escape paths).
    void* carve_from_arena(std::size_t slot_size) {
        std::size_t need = round_up(sizeof(BlockHeader) + slot_size, alignof(std::max_align_t));
        std::lock_guard<std::mutex> lock(arena_mu_);
        if (bump_ + need > arena_bytes_) return nullptr;
        std::byte* base = arena_.data() + bump_;
        bump_ += need;
        return base + sizeof(BlockHeader);
    }

    void* allocate_escaped(std::size_t bytes) {
        // Raw ::operator new for the header+payload; tagged ESCAPED.
        void* raw = operator new(sizeof(BlockHeader) + bytes);
        BlockHeader* h = reinterpret_cast<BlockHeader*>(raw);
        init_header(h, ESCAPED, static_cast<uint32_t>(bytes), bytes);
        stats_.live.fetch_add(1, std::memory_order_relaxed);
        stats_.escaped.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<std::byte*>(raw) + sizeof(BlockHeader);
    }

    void recycle_to_bin(BlockHeader* h) {
        std::size_t idx = h->size_class;
        if (idx >= bins_.size()) {
            // Should not happen for non-escaped; be defensive.
            operator delete(h);
            return;
        }
        Bin& bin = *bins_[idx];
        std::lock_guard<std::mutex> lock(bin.mutex);
        // Poison magic to detect stale use while on the free-list.
        h->magic = 0;
        push_free(bin, reinterpret_cast<std::byte*>(h) + sizeof(BlockHeader));
    }

    static std::size_t round_up(std::size_t n, std::size_t align) {
        return (n + align - 1) & ~(align - 1);
    }

    std::vector<uint32_t> classes_;
    std::vector<std::unique_ptr<Bin>> bins_;
    std::vector<std::byte> arena_;
    std::size_t arena_bytes_;
    std::size_t bump_;
    std::mutex arena_mu_;
    AtomicStats stats_;
};

// RAII handle to a pool allocation. Copies retain (bump refcount); moves
// transfer ownership; destruction releases.
template <class T>
class PoolPtr {
public:
    PoolPtr() : ptr_(nullptr), pool_(nullptr) {}
    PoolPtr(std::nullptr_t) : ptr_(nullptr), pool_(nullptr) {}

    // Takes ownership of a refcount==1 allocation produced by the pool.
    PoolPtr(MemoryPool* pool, T* p) : ptr_(p), pool_(pool) {}

    PoolPtr(const PoolPtr& other) : ptr_(other.ptr_), pool_(other.pool_) {
        if (pool_ && ptr_) pool_->retain(ptr_);
    }

    PoolPtr(PoolPtr&& other) noexcept : ptr_(other.ptr_), pool_(other.pool_) {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
    }

    PoolPtr& operator=(const PoolPtr& other) {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            if (pool_ && ptr_) pool_->retain(ptr_);
        }
        return *this;
    }

    PoolPtr& operator=(PoolPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    ~PoolPtr() { reset(); }

    T& operator*() const { return *ptr_; }
    T* operator->() const { return ptr_; }
    T* get() const { return ptr_; }
    MemoryPool* pool() const { return pool_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() {
        if (pool_ && ptr_) {
            pool_->release(ptr_);
        }
        ptr_ = nullptr;
        pool_ = nullptr;
    }

private:
    T* ptr_;
    MemoryPool* pool_;
};

// Construct a T inside the pool via placement new. Returns a PoolPtr<T> with
// refcount 1.
template <class T, class... Args>
PoolPtr<T> make_pool(MemoryPool& pool, Args&&... args) {
    void* mem = pool.allocate(sizeof(T));
    try {
        T* obj = new (mem) T(std::forward<Args>(args)...);
        return PoolPtr<T>(&pool, obj);
    } catch (...) {
        pool.release(mem);
        throw;
    }
}

// Destroy a T previously built with make_pool and release its memory.
template <class T>
void destroy_pool(PoolPtr<T>& p) {
    if (p) {
        p.get()->~T();
        p.reset();
    }
}

// A growable byte buffer backed by a MemoryPool allocation. Designed for
// per-connection read buffers that are reused across many requests on the same
// connection (HTTP keep-alive): each request resets len() to 0 but keeps the
// underlying block, so N requests cost ~1 pool allocation instead of N
// malloc/free cycles.
//
// Lifetime: owns one pool block (raw void*, refcount 1) via the pool. On grow,
// if the new size exceeds the current block's slot size, the old block is
// released and a larger one allocated (the pool's ESCAPED path handles sizes
// beyond the largest class). On destruction the block is released back.
//
// scan_pos() is the caller's bookkeeping for incremental delimiter search
// (avoids re-scanning the whole buffer on every read); PooledBuffer just
// stores and exposes it.
class PooledBuffer {
public:
    PooledBuffer() = default;

    explicit PooledBuffer(MemoryPool& pool, std::size_t initial_cap = 4096)
    : pool_(&pool) {
        reserve(initial_cap);
    }

    // Non-copyable (owns a pool allocation); movable.
    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;

    PooledBuffer(PooledBuffer&& o) noexcept
    : pool_(o.pool_), data_(o.data_), cap_(o.cap_), len_(o.len_),
      scan_pos_(o.scan_pos_) {
        o.pool_ = nullptr;
        o.data_ = nullptr;
        o.cap_ = 0;
        o.len_ = 0;
        o.scan_pos_ = 0;
    }

    PooledBuffer& operator=(PooledBuffer&& o) noexcept {
        if (this != &o) {
            release_pool_block();
            len_ = 0;
            scan_pos_ = 0;
            pool_ = o.pool_;
            data_ = o.data_;
            cap_ = o.cap_;
            len_ = o.len_;
            scan_pos_ = o.scan_pos_;
            o.pool_ = nullptr;
            o.data_ = nullptr;
            o.cap_ = 0;
            o.len_ = 0;
            o.scan_pos_ = 0;
        }
        return *this;
    }

    ~PooledBuffer() { release_pool_block(); }

    // Ensure at least `need` bytes of usable capacity. Reallocates via the
    // pool (release old, allocate new) only when growing past the current slot.
    void reserve(std::size_t need) {
        if (need <= cap_) return;
        // Allocate the new (larger) block first, copy, then release the old.
        void* fresh = pool_->allocate(need);
        if (fresh == nullptr) return;  // allocation failed; keep old block
        // Copy live data over before releasing the old block. We save len_
        // because release_pool_block() zeroes it.
        std::size_t saved_len = len_;
        if (data_ != nullptr && saved_len > 0) {
            std::memcpy(fresh, data_, saved_len);
        }
        release_pool_block();
        data_ = static_cast<std::byte*>(fresh);
        cap_ = need;
        len_ = saved_len;
    }

    // Append bytes, growing if necessary. Returns false on allocation failure.
    bool append(const void* src, std::size_t n) {
        if (len_ + n > cap_) {
            // Grow geometrically to amortize, but never smaller than what's
            // needed.
            std::size_t newcap = cap_ == 0 ? 4096 : cap_;
            while (newcap < len_ + n) newcap *= 2;
            reserve(newcap);
            if (len_ + n > cap_) return false;  // grow failed
        }
        std::memcpy(static_cast<std::byte*>(data_) + len_, src, n);
        len_ += n;
        return true;
    }

    // Reset for reuse on the same connection (keep the block). Clears length
    // and the scan position.
    void reset_for_reuse() {
        len_ = 0;
        scan_pos_ = 0;
    }

    // Set the live length directly. Used after manually shifting bytes in the
    // buffer (e.g. discarding a consumed request and compacting the leftover
    // pipelined bytes to the front). Caller is responsible for only shrinking
    // or having written valid data into [0, n).
    void set_size(std::size_t n) { len_ = n; }

    void* data() { return data_; }
    const void* data() const { return data_; }
    std::size_t size() const { return len_; }
    std::size_t capacity() const { return cap_; }
    bool empty() const { return len_ == 0; }

    std::size_t scan_pos() const { return scan_pos_; }
    void set_scan_pos(std::size_t p) { scan_pos_ = p; }

    explicit operator bool() const { return data_ != nullptr; }

private:
    // Return the current block to its pool free-list and forget it. Does NOT
    // touch len_/scan_pos_ — callers (reserve, move, destructor) manage those.
    void release_pool_block() {
        if (pool_ && data_) {
            pool_->release(data_);
        }
        data_ = nullptr;
        cap_ = 0;
    }

    MemoryPool* pool_ = nullptr;
    void* data_ = nullptr;   // usable region (pool allocate returns this)
    std::size_t cap_ = 0;    // usable bytes available at data_
    std::size_t len_ = 0;    // bytes currently filled
    std::size_t scan_pos_ = 0;  // caller-managed delimiter scan cursor
};

}  // namespace muses

#endif  // MUSES_MEMORY_POOL_HPP
