#pragma once

#include <iostream>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <string>
#include <atomic>
#include <utility>
#include <new>
#include "concepts.hpp"

#if defined(__APPLE__) && defined(__arm64__)
constexpr size_t HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE = 128;
#else
constexpr size_t HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE = 64;
#endif

alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) std::atomic<size_t> write_idx{0};
alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) std::atomic<size_t> read_idx{0};

static constexpr uint32_t NULL_IDX = 0xFFFFFFFF;
static constexpr size_t CHUNK_SIZE = 64; 
static constexpr size_t QUEUE_CAPACITY = 512;

template<ValidMarketPacket PacketType>
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

    inline void return_read_cache() { read_index_cached = read_idx.load(std::memory_order_acquire); }
    inline void return_write_cache() { write_index_cached = write_idx.load(std::memory_order_acquire); }

public:
    inline T* get_write_slot() {
        const size_t curr_write = write_idx.load(std::memory_order_relaxed);
        const size_t curr_read = read_index_cached;
        
        if ((curr_write - curr_read) == Capacity) [[unlikely]] {
            return_read_cache();
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
        const size_t curr_write = write_index_cached;

        if (curr_read == curr_write) {
            return_write_cache();
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

static constexpr int MIN_PRICE = 900;
static constexpr int MAX_PRICE = 1100;
static constexpr int LEVELS = MAX_PRICE - MIN_PRICE + 1;
inline int idx(int price) { return price - MIN_PRICE; }
inline int price(int idx) { return idx + MIN_PRICE; }

template<int Levels>
class Pricemap {
private:
    static constexpr size_t BLOCKS = (Levels + 63) / 64;
    uint64_t mask_level1[BLOCKS]{0};
    uint64_t mask_level2 = 0;

public:
    inline void set(int idx) {
        const size_t l1_idx = static_cast<size_t>(idx) >> 6;
        mask_level1[l1_idx] |= (1ULL << (idx & 63));
        mask_level2 |= (1ULL << l1_idx); 
    }
    inline void clear(int idx) {
        const size_t l1_idx = static_cast<size_t>(idx) >> 6;
        mask_level1[l1_idx] &= ~(1ULL << (idx & 63));
        if(!mask_level1[l1_idx]) mask_level2 &= ~(1ULL << l1_idx);
    }
    inline int find_max() const {
        if (!mask_level2) return -1;
        const size_t l2_bit = 63 - __builtin_clzll(mask_level2);
        const size_t l1_bit = 63 - __builtin_clzll(mask_level1[l2_bit]);
        return (l2_bit << 6) | l1_bit;
    }
    inline int find_min() const {
        if (!mask_level2) return -1;
        const size_t l2_bit = __builtin_ctzll(mask_level2);
        const size_t l1_bit = __builtin_ctzll(mask_level1[l2_bit]);
        return (l2_bit << 6) | l1_bit;
    }
    inline void clear_less(int idx) {
        if (idx < 0) [[unlikely]] return;
        if (idx >= Levels - 1) [[unlikely]] {
            for (size_t i = 0; i < BLOCKS; ++i) mask_level1[i] = 0;
            mask_level2 = 0;
            return;
        }
        const size_t l1_idx = static_cast<size_t>(idx) >> 6;
        const size_t l1_bit = static_cast<size_t>(idx) & 63;
        for (size_t i = 0; i < l1_idx; ++i) mask_level1[i] = 0;
        mask_level1[l1_idx] &= (l1_bit == 63) ? 0ULL : (~0ULL << (l1_bit + 1));
        mask_level2 = (mask_level2 & ((l1_idx >= 63) ? 0ULL : (~0ULL << (l1_idx + 1)))) | (static_cast<uint64_t>(mask_level1[l1_idx] != 0) << l1_idx);
    }
    inline void clear_great(int idx) {
        if (idx <= 0) [[unlikely]] {
            if (idx == 0) {
                for (size_t i = 0; i < BLOCKS; ++i) mask_level1[i] = 0;
                mask_level2 = 0;
            }
            return;
        }
        if (idx >= Levels) [[unlikely]] return;
        const size_t l1_idx = static_cast<size_t>(idx) >> 6;
        const size_t l1_bit = static_cast<size_t>(idx) & 63;
        for (size_t i = l1_idx + 1; i < BLOCKS; ++i) mask_level1[i] = 0;
        mask_level1[l1_idx] &= (1ULL << l1_bit) - 1ULL;
        mask_level2 = (mask_level2 & ((1ULL << l1_idx) - 1ULL)) | (static_cast<uint64_t>(mask_level1[l1_idx] != 0) << l1_idx);
    }
};

struct Snapshot {
    int bid_price = -1;
    uint32_t bid_qty = 0;
    int ask_price = -1;
    uint32_t ask_qty = 0;
    inline int spread() const {
        if (bid_price != -1 && ask_price != -1 && ask_price >= bid_price) return ask_price - bid_price;
        return -1;
    }

    inline double mid_price() const {
        if (bid_price != -1 && ask_price != -1) return (bid_price + ask_price) * 0.5;
        return 0.0;
    }
};

enum Side : bool { Sell = false, Buy = true };

class OrderBook {
    Pricemap<LEVELS> bid_mask;
    Pricemap<LEVELS> ask_mask;
    int best_bid_idx = -1;
    int best_ask_idx = -1;
    uint32_t bids_qty[LEVELS]{0};
    uint32_t asks_qty[LEVELS]{0};
    template<Side S>
    [[gnu::always_inline]] void update_level(int price, uint32_t qty) {
        constexpr bool IsBuy = (S == Side::Buy);
        const int p_idx = idx(price);
        uint32_t* side = (IsBuy ? bids_qty : asks_qty);
        auto& mask = (IsBuy ? bid_mask : ask_mask);
        side[p_idx] = qty;
        if (qty > 0) {
            mask.set(p_idx);
            if constexpr (IsBuy) {
                if (best_ask_idx != -1 && best_ask_idx <= p_idx) {
                    ask_mask.clear_less(p_idx);
                    memset(&asks_qty[best_ask_idx] , 0 , (p_idx - best_ask_idx + 1) * sizeof(uint32_t));
                    best_ask_idx = ask_mask.find_min();
                }
                best_bid_idx = std::max(best_bid_idx , p_idx);
            } 
            else {
                if (best_bid_idx != -1 && best_bid_idx >= p_idx) {
                    bid_mask.clear_great(p_idx);
                    memset(&bids_qty[p_idx] , 0 , (best_bid_idx - p_idx + 1) * sizeof(uint32_t));
                    best_bid_idx = bid_mask.find_max();
                }
                if (best_ask_idx == -1 || p_idx < best_ask_idx) best_ask_idx = p_idx;
            }
        }
        else {
            mask.clear(p_idx);
            if constexpr (IsBuy) {
                if (p_idx == best_bid_idx) [[unlikely]] best_bid_idx = bid_mask.find_max();
            } else {
                if (p_idx == best_ask_idx) [[unlikely]] best_ask_idx = ask_mask.find_min();
            }
        }
    }
    /*
    template<Side S>
    [[gnu::always_inline]] void delta_level(int price, uint32_t qty) {
        constexpr bool IsBuy = (S == Side::Buy);
        const int p_idx = idx(price);
        uint32_t* side = (IsBuy ? bids_qty : asks_qty);
        auto& mask = (IsBuy ? bid_mask : ask_mask);
        const int64_t updated_qty = static_cast<int64_t>(side[p_idx]) + qty;
        if (updated_qty > 0) [[likely]] {
            side[p_idx] = static_cast<uint32_t>(updated_qty);
            mask.set(p_idx);
        }
        else {
            side[p_idx] = 0;
            mask.clear(p_idx);
        }
    }
    */
public:
    inline int get_best_bid_price() const { 
        return best_bid_idx == -1 ? -1 : best_bid_idx + MIN_PRICE;
    }
    inline uint32_t get_best_bid_qty() const {
        return best_bid_idx == -1 ? 0 : bids_qty[best_bid_idx];
    }
    inline int get_best_ask_price() const { 
        return best_ask_idx == -1 ? -1 : best_ask_idx + MIN_PRICE; 
    }
    inline uint32_t get_best_ask_qty() const {
        return best_ask_idx == -1 ? 0 : asks_qty[best_ask_idx];
    }
    inline Snapshot get_snapshot() const {
        Snapshot snap;
        if (best_bid_idx != -1) {
            snap.bid_price = price(best_bid_idx);
            snap.bid_qty = bids_qty[best_bid_idx];
        }
        if (best_ask_idx != -1) {
            snap.ask_price = price(best_ask_idx);
            snap.ask_qty = asks_qty[best_ask_idx];
        }
        return snap;
    }
    template<Side side>
    [[gnu::always_inline]] inline void add_in_limit(int price , uint32_t qty){
        if(price < MIN_PRICE || price > MAX_PRICE) [[unlikely]] return;
        update_level<side>(price, qty);
    }
};