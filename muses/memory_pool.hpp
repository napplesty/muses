#pragma once

#ifndef _MUSES_MEMORY_POOL
#define _MUSES_MEMORY_POOL

#include <mutex>

namespace muses {

struct memory_block {
    bool is_allocated;
    unsigned char *data;
};
typedef struct memory_block memory_block_t;

class MemoryPool {
public:
    explicit MemoryPool(unsigned int block_size, unsigned int num_blocks):
    block_size(block_size),
    num_blocks(num_blocks),
    memory_pool(nullptr) {}

    ~MemoryPool() {
        if (memory_pool != nullptr) {
            delete [] memory_pool;
        }
    }

    void initialize() {
        if (memory_pool == nullptr) {
            std::lock_guard<std::mutex> lock(initial_mutex);
            if (memory_pool == nullptr) {
                memory_pool = new memory_block_t[num_blocks];
                for(unsigned int i = 0; i < num_blocks; i++) {
                    memory_pool[i].is_allocated = false;
                    memory_pool[i].data = new unsigned char[block_size];
                }
            }
        }
    }
    
    void *allocate() {
        std::lock_guard<std::mutex> lock(rw_mutex);
        for(unsigned int i = 0; i < num_blocks; i++) {
            if (!memory_pool[i].is_allocated) {
                memory_pool[i].is_allocated = true;
                return memory_pool[i].data;
            }
        }
        return nullptr;
    }

    void deallocate(void *ptr) {
        std::lock_guard<std::mutex> lock(rw_mutex);
        for(unsigned int i = 0; i < num_blocks; i++) {
            if (memory_pool[i].data == ptr) {
                memory_pool[i].is_allocated = false;
                return;
            }
        }
    }

public:
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

private:
    unsigned int block_size;
    unsigned int num_blocks;
    memory_block_t *memory_pool;
    std::mutex rw_mutex;
    std::mutex initial_mutex;
};

};
#endif