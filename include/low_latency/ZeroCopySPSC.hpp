#pragma once

#include <cstddef>
#include <atomic>

#if defined(__APPLE__) && defined(__arm64__)
constexpr size_t HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE = 128;
#else
constexpr size_t HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE = 64;
#endif

static constexpr size_t CHUNK_SIZE = 64; 
static constexpr size_t QUEUE_CAPACITY = 512;

template<typename PacketType>
struct alignas(64) DataChunk {
    PacketType packets[CHUNK_SIZE];
    size_t valid_count = 0;
};

template<typename T, size_t Capacity>
class ZeroCopySPSC {
private:
    static_assert((Capacity & (Capacity - 1)) == 0 , "Capacity must be power of 2");

    alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) std::atomic<size_t> write_idx{0};
    alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) size_t write_index_cached = 0;
    
    alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) std::atomic<size_t> read_idx{0};
    alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) size_t read_index_cached = 0;

    alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) T buffer[Capacity];

    inline void refresh_read_cache() { read_index_cached = read_idx.load(std::memory_order_acquire); }
    inline void refresh_write_cache() { write_index_cached = write_idx.load(std::memory_order_acquire); }

public:
    inline T* get_write_slot() {
        const size_t curr_write = write_idx.load(std::memory_order_relaxed);
        if ((curr_write - read_index_cached) == Capacity) [[unlikely]] {
            refresh_read_cache();
            if ((curr_write - read_index_cached) == Capacity) return nullptr;
        }
        return &buffer[curr_write & (Capacity - 1)];
    }

    inline void commit_write() {
        size_t curr_write = write_idx.load(std::memory_order_relaxed);
        write_idx.store(curr_write + 1, std::memory_order_release);
    }

    inline const T* peek_read_slot() {
        const size_t curr_read = read_idx.load(std::memory_order_relaxed);
        if (curr_read == write_index_cached) {
            refresh_write_cache();
            if (curr_read == write_index_cached) return nullptr;
        }
        return &buffer[curr_read & (Capacity - 1)];
    }

    inline const T* peek_next_read_slot() const {
        const size_t curr_read = read_idx.load(std::memory_order_relaxed);
        return &buffer[(curr_read + 1) & (Capacity - 1)];
    }

    inline void commit_read() {
        size_t curr_read = read_idx.load(std::memory_order_relaxed);
        read_idx.store(curr_read + 1, std::memory_order_release);
    }
};