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

#ifndef _MUSES_MEMORY_POOL_HPP
#define _MUSES_MEMORY_POOL_HPP

#include <mutex>
#include "muses/compiler_defs.hpp"
#include "muses/logging.hpp"

namespace muses {

typedef enum {
    ALLOCATABLE,
    DEALLOCATABLE,
    DEALLOCATING,
} memory_block_status_t;

typedef struct buffer_info {
    unsigned int length;
    unsigned int buf_begin;
} buffer_info_t;

typedef struct memory_block {
    memory_block_status_t status;
    unsigned int offset;
    unsigned int ref_count;
    unsigned char *data;
} memory_block_t;

class MemoryPool {
public:
    explicit MemoryPool(unsigned int block_size, unsigned int num_blocks):
    block_size(block_size),
    num_blocks(num_blocks),
    memory_pool(nullptr),
    allocating_block_front(0),
    deallocating_threshold(2),
    deallocating_count(0),
    failed_count(0) {}

    ~MemoryPool() {
        if (memory_pool != nullptr) {
            delete [] memory_pool_data;
            delete [] memory_pool;
        }
    }

    void initialize() {
        if (memory_pool == nullptr) {
            std::lock_guard<std::mutex> lock(rw_mutex);
            if (memory_pool == nullptr) {
                memory_pool = new memory_block_t[num_blocks];
                memory_pool_data = new unsigned char[block_size*num_blocks];
                for(unsigned int i = 0; i < num_blocks; i++) {
                    memory_pool[i].status = ALLOCATABLE;
                    memory_pool[i].ref_count = 0;
                    memory_pool[i].data = memory_pool_data + i*block_size;
                }
            }
        }
    }
    
    void *allocate(size_t size) {
        std::lock_guard<std::mutex> lock(rw_mutex);
        for(int i = 0; i < num_blocks; i++) {
            int block_idx = (i + allocating_block_front) % num_blocks;
            if (memory_pool[block_idx].status == DEALLOCATABLE) {
                memory_pool[block_idx].status = ALLOCATABLE;
                memory_pool[block_idx].offset = 0;
                memory_pool[block_idx].ref_count = 0;
            } else if (memory_pool[block_idx].status != ALLOCATABLE) {
                continue;
            }
            if (check_allocatable(size, block_idx)) {
                int offset = memory_pool[block_idx].offset;
                int *buffer_info_size = reinterpret_cast<int *>(memory_pool[block_idx].data + offset);
                *buffer_info_size = size;
                memory_pool[block_idx].offset += size + sizeof(buffer_info_t::length);
                memory_pool[block_idx].ref_count ++;
                return static_cast<void *>(buffer_info_size + 1);
            } else {
                deallocating_count ++;
                if (deallocating_count > deallocating_threshold) {
                    memory_pool[allocating_block_front].status = DEALLOCATING;
                    allocatable_step();
                    deallocating_count = 0;
                }
            }
        }
        failed_count ++;
        return new unsigned char[size];
    }

    void deallocate(void *ptr) {
        std::lock_guard<std::mutex> lock(rw_mutex);
        if( reinterpret_cast<long>(ptr) > reinterpret_cast<long>(memory_pool_data + num_blocks * block_size) || 
            reinterpret_cast<long>(ptr) < reinterpret_cast<long>(memory_pool_data)) {
            delete [] reinterpret_cast<unsigned char *>(ptr);
            return;
        }
        int buffer_idx = (reinterpret_cast<unsigned char *>(ptr)-memory_pool_data) / block_size;
        if (memory_pool[buffer_idx].status == ALLOCATABLE) {
            memory_pool[buffer_idx].status = DEALLOCATING;
            allocatable_step();
        } else if (memory_pool[buffer_idx].status == DEALLOCATABLE) {
            MUSES_ERROR("deallocated failed.");
            return;
        }
        
        memory_pool[buffer_idx].ref_count --;
        if(memory_pool[buffer_idx].ref_count == 0) {
            memory_pool[buffer_idx].status = DEALLOCATABLE;
        }
    }

private:
    inline bool check_allocatable(size_t size, int block_idx) {
        size_t allocate_size = size + sizeof(buffer_info_t::length);
        return block_size - memory_pool->offset >= allocate_size;
    }

    inline void allocatable_step() {
        allocating_block_front = (allocating_block_front + 1) % num_blocks;
    }

public:
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

private:
    unsigned int block_size;
    unsigned int num_blocks;
    memory_block_t *memory_pool;
    unsigned char *memory_pool_data;
    std::mutex rw_mutex;

    int allocating_block_front;
    int deallocating_threshold;
    int deallocating_count;
    long long failed_count;
};

};
#endif