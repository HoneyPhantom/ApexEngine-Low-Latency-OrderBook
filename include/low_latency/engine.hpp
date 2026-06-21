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

    alignas(64) std::atomic<size_t> write_idx{0};
    alignas(64) size_t write_index_cached = 0;
    
    alignas(64) std::atomic<size_t> read_idx{0};
    alignas(64) size_t read_index_cached = 0;

    alignas(64) T buffer[Capacity];

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

    inline void commit_read() {
        size_t curr_read = read_idx.load(std::memory_order_relaxed);
        read_idx.store(curr_read + 1, std::memory_order_release);
    }
};

inline void hardware_spin_relax() {
#if defined(__ARM_ARCH) || defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    asm volatile("pause" ::: "memory");
#endif
}

template<typename T , size_t POOL_SIZE>
class MemoryPool {
private:
    alignas(64) char buffer[POOL_SIZE * sizeof(T)];
    struct FreeNode { uint32_t next_idx; };
    uint32_t free_head = 0;
    
    inline T* get_slot(size_t idx) { return reinterpret_cast<T*>(&buffer[idx * sizeof(T)]); }
    inline const T* get_slot(size_t idx) const { return reinterpret_cast<const T*>(&buffer[idx * sizeof(T)]); }
public:
    MemoryPool() {
        for(size_t i = 0; i < POOL_SIZE - 1; ++i) reinterpret_cast<FreeNode*>(get_slot(i))->next_idx = static_cast<uint32_t>(i + 1);
        reinterpret_cast<FreeNode*>(get_slot(POOL_SIZE - 1))->next_idx = static_cast<uint32_t>(NULL_IDX);
    }

    uint32_t alloc_raw(){
        if(free_head == NULL_IDX) [[unlikely]] return NULL_IDX;
        uint32_t node = free_head;
        free_head = reinterpret_cast<FreeNode*>(get_slot(node))->next_idx;
        return node;
    }

    void dealloc_raw(uint32_t idx){
        if(idx == NULL_IDX || idx >= POOL_SIZE) return;
        reinterpret_cast<FreeNode*>(get_slot(idx))->next_idx = free_head;
        free_head = idx;
    }

    template <class... Args>
    uint32_t create(Args&&... args) {
        uint32_t idx = alloc_raw();
        if (idx == NULL_IDX) return NULL_IDX;
        new (get_slot(idx)) T(std::forward<Args>(args)...);
        return idx;
    }

    void destroy(uint32_t idx) {
        if(idx == NULL_IDX || idx >= POOL_SIZE) return;
        get_slot(idx)->~T();
        dealloc_raw(idx);
    }

    inline T& operator[](size_t idx) { return const_cast<T&>(static_cast<const MemoryPool&>(*this)[idx]); }
    inline const T& operator[](size_t idx) const { return *get_slot(idx); }
    inline T* get_base_ptr() { return reinterpret_cast<T*>(buffer); }
};

struct Order {
    uint32_t id;
    uint32_t qty;
    int price;
    uint32_t next_idx;
    Order(uint32_t id_, uint32_t qty_, int price_) : id(id_), qty(qty_), price(price_), next_idx(NULL_IDX) {}
};

struct Level {
    uint32_t head = NULL_IDX;
    uint32_t tail = NULL_IDX;

    inline void push(Order* pool , uint32_t new_order_idx){
        pool[new_order_idx].next_idx = NULL_IDX;
        if(tail == NULL_IDX) head = tail = new_order_idx;
        else {
            pool[tail].next_idx = new_order_idx;
            tail = new_order_idx;
        }
    }

    inline uint32_t pop(Order* pool){
        if(head == NULL_IDX) return NULL_IDX;
        uint32_t x = head;
        head = pool[x].next_idx;
        if(empty()) tail = NULL_IDX;
        return x;
    }
    inline uint32_t top() const { return head; }
    inline bool empty() const { return head == NULL_IDX; }
};

static constexpr int MIN_PRICE = 900;
static constexpr int MAX_PRICE = 1100;
static constexpr int LEVELS = MAX_PRICE - MIN_PRICE + 1;
inline int idx(int price){ return price - MIN_PRICE; }

template<int Levels>
class Pricemap {
private:
    static constexpr size_t BLOCKS1 = (Levels + 63) / 64;
    static constexpr size_t BLOCKS2 = (BLOCKS1 + 63) / 64;
    uint64_t mask_level1[BLOCKS1] = {0};
    uint64_t mask_level2[BLOCKS2] = {0};

public:
    inline void set(int idx) {
        const int l1_idx = idx >> 6, l2_idx = l1_idx >> 6;
        mask_level1[l1_idx] |= (1ULL << (idx & 63));
        mask_level2[l2_idx] |= (1ULL << (l1_idx & 63)); 
    }
    inline void clear(int idx) {
        const int l1_idx = idx >> 6, l2_idx = l1_idx >> 6;
        mask_level1[l1_idx] &= ~(1ULL << (idx & 63));
        if(!mask_level1[l1_idx]) {
            mask_level2[l2_idx] &= ~(1ULL << (l1_idx & 63));
        }
    }
    inline int find_max() const {
        if constexpr (BLOCKS2 == 1) {
            if (mask_level2[0]) {
                int l2_bit = 63 - __builtin_clzll(mask_level2[0]);
                return (l2_bit << 6) | (63 - __builtin_clzll(mask_level1[l2_bit]));
            }
        } 
        else {
            for(int i = static_cast<int>(BLOCKS2) - 1; i >= 0; --i) {
                if (mask_level2[i]) {
                    int l2_bit = 63 - __builtin_clzll(mask_level2[i]);
                    int block_idx = (i << 6) | l2_bit;
                    return (block_idx << 6) | (63 - __builtin_clzll(mask_level1[block_idx]));
                }
            }
        }
        return -1;
    }
    inline int find_min() const {
        if constexpr (BLOCKS2 == 1) {
            if (mask_level2[0]) {
                int l2_bit = __builtin_ctzll(mask_level2[0]);
                return (l2_bit << 6) | __builtin_ctzll(mask_level1[l2_bit]);
            }
        } 
        else {
            for(size_t i = 0; i < BLOCKS2; ++i) {
                if (mask_level2[i]) {
                    int l2_bit = __builtin_ctzll(mask_level2[i]);
                    size_t block_idx = (i << 6) | l2_bit;
                    return static_cast<int>((block_idx << 6) | (__builtin_ctzll(mask_level1[block_idx])));
                }
            }
        }
        return -1;
    }
};

enum Side : bool { Sell = false, Buy = true };

class alignas(64) OrderBook {
private:
    alignas(64) Level bids[LEVELS];
    alignas(64) Level asks[LEVELS];
    MemoryPool<Order , 1000000> pool;
    uint32_t next_idx = 1;
    Pricemap<LEVELS> bid_mask, ask_mask;

    inline uint32_t branchless_min(const uint32_t& a , const uint32_t& b){
        return b + ((a - b) & ((int32_t)(a - b) >> 31));
    }
    inline void enqueue_bid(int price , uint32_t qty){
        uint32_t id = pool.create(next_idx++ , qty , price);
        if(id == NULL_IDX) [[unlikely]] return;
        int price_idx = idx(price);
        bids[price_idx].push(get_raw_pool() , id);
        bid_mask.set(price_idx);
    }
    inline void enqueue_ask(int price , uint32_t qty){
        uint32_t id = pool.create(next_idx++ , qty , price);
        if(id == NULL_IDX) [[unlikely]] return;
        int price_idx = idx(price);
        asks[price_idx].push(get_raw_pool() , id);
        ask_mask.set(price_idx);
    }
    
    template<Side S>
    [[gnu::always_inline]] void handle_order(int price, uint32_t qty) {
        constexpr bool IsBuy = (S == Side::Buy);
        Order* raw_pool = get_raw_pool();
        auto& target_levels = *(IsBuy ? &asks : &bids);
        auto& target_mask  = IsBuy ? ask_mask : bid_mask;
        while(qty > 0){
            int best_idx = IsBuy ? target_mask.find_min() : target_mask.find_max();
            if(best_idx == -1 || (IsBuy ? (best_idx + MIN_PRICE > price) : (best_idx + MIN_PRICE < price))) break;
            auto& lvl = target_levels[best_idx];
            while(qty > 0 && !lvl.empty()){
                uint32_t id = lvl.top();
                Order& o = pool[id];
                uint32_t traded = branchless_min(o.qty, qty);
                o.qty -= traded;
                qty -= traded;
                if(o.qty == 0) pool.destroy(lvl.pop(raw_pool));
            }
            if(lvl.empty()) target_mask.clear(best_idx);
        }
        if(qty > 0) {
            if constexpr (IsBuy) enqueue_bid(price, qty);
            else enqueue_ask(price, qty);
        }
    }
public:
    inline Order* get_raw_pool() { return pool.get_base_ptr(); }
    inline int get_best_bid() const{ 
        int idx = bid_mask.find_max();
        return idx == -1 ? -1 : idx + MIN_PRICE;
    }
    inline int get_best_ask() const{ 
        int idx = ask_mask.find_min();
        return idx == -1 ? -1 : idx + MIN_PRICE; 
    }
    void add_in_limit(bool is_buy , int price , uint32_t qty){
        if(price < MIN_PRICE || price > MAX_PRICE) [[unlikely]] return;
        if(is_buy) handle_order<Side::Buy>(price, qty);
        else handle_order<Side::Sell>(price, qty);
    }
};