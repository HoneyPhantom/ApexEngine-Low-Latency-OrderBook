#pragma once

#include <iostream>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <array>
#include <span>
#include <chrono>
#include "concepts.hpp"

static constexpr int MIN_PRICE = 900;
static constexpr int MAX_PRICE = 1100;
static constexpr int LEVELS = MAX_PRICE - MIN_PRICE + 1;
static constexpr uint32_t NULL_IDX = 0xFFFFFFFF;

static constexpr size_t DEFAULT_MAX_ORDERS = 4'194'304; 
static constexpr size_t DIRECT_MAP_CAPACITY = 8'388'608;

[[gnu::always_inline]] inline int idx(int price) { return price - MIN_PRICE; }
[[gnu::always_inline]] inline int price(int idx) { return idx + MIN_PRICE; }

template<int Levels>
class Pricemap {
private:
    static constexpr size_t BLOCKS = (Levels + 63) / 64;
    uint64_t mask_level1[BLOCKS]{0};
    uint64_t mask_level2 = 0;

public:
    [[gnu::always_inline]] inline void set(int idx) {
        const size_t l1_idx = static_cast<size_t>(idx) >> 6;
        mask_level1[l1_idx] |= (1ULL << (idx & 63));
        mask_level2 |= (1ULL << l1_idx); 
    }

    [[gnu::always_inline]] inline void clear(int idx) {
        const size_t l1_idx = static_cast<size_t>(idx) >> 6;
        mask_level1[l1_idx] &= ~(1ULL << (idx & 63));
        if (!mask_level1[l1_idx]) mask_level2 &= ~(1ULL << l1_idx);
    }

    [[gnu::always_inline]] inline int find_max() const {
        if (!mask_level2) return -1;
        const size_t l2_bit = 63 - __builtin_clzll(mask_level2);
        const size_t l1_bit = 63 - __builtin_clzll(mask_level1[l2_bit]);
        return (l2_bit << 6) | l1_bit;
    }

    [[gnu::always_inline]] inline int find_min() const {
        if (!mask_level2) return -1;
        const size_t l2_bit = __builtin_ctzll(mask_level2);
        const size_t l1_bit = __builtin_ctzll(mask_level1[l2_bit]);
        return (l2_bit << 6) | l1_bit;
    }

    [[gnu::always_inline]] inline int find_prev(int current_idx) const {
        if (current_idx <= 0 || !mask_level2) return -1;
        size_t l1_idx = static_cast<size_t>(current_idx) >> 6;
        size_t bit = static_cast<size_t>(current_idx) & 63;

        uint64_t lower_bits = mask_level1[l1_idx] & ((1ULL << bit) - 1ULL);
        if (lower_bits != 0) {
            return (l1_idx << 6) | (63 - __builtin_clzll(lower_bits));
        }

        uint64_t lower_blocks = mask_level2 & ((1ULL << l1_idx) - 1ULL);
        if (!lower_blocks) return -1;

        size_t prev_l1_idx = 63 - __builtin_clzll(lower_blocks);
        return (prev_l1_idx << 6) | (63 - __builtin_clzll(mask_level1[prev_l1_idx]));
    }

    [[gnu::always_inline]] inline int find_next(int current_idx) const {
        if (current_idx >= Levels - 1 || !mask_level2) return -1;
        size_t l1_idx = static_cast<size_t>(current_idx) >> 6;
        size_t bit = static_cast<size_t>(current_idx) & 63;

        uint64_t higher_bits = (bit == 63) ? 0ULL : (mask_level1[l1_idx] & (~0ULL << (bit + 1)));
        if (higher_bits != 0) {
            return (l1_idx << 6) | __builtin_ctzll(higher_bits);
        }

        uint64_t higher_blocks = (l1_idx >= 63) ? 0ULL : (mask_level2 & (~0ULL << (l1_idx + 1)));
        if (!higher_blocks) return -1;

        size_t next_l1_idx = __builtin_ctzll(higher_blocks);
        return (next_l1_idx << 6) | __builtin_ctzll(mask_level1[next_l1_idx]);
    }
};

template<typename T, size_t POOL_SIZE>
class MemoryPool {
private:
    alignas(64) T buffer[POOL_SIZE];
    struct FreeNode { uint32_t next_idx; };
    uint32_t free_head = 0;

public:
    MemoryPool() {
        for (size_t i = 0; i < POOL_SIZE - 1; ++i) {
            reinterpret_cast<FreeNode*>(&buffer[i])->next_idx = static_cast<uint32_t>(i + 1);
        }
        reinterpret_cast<FreeNode*>(&buffer[POOL_SIZE - 1])->next_idx = NULL_IDX;
    }

    [[gnu::always_inline]] inline uint32_t alloc_raw() {
        if (free_head == NULL_IDX) [[unlikely]] return NULL_IDX;
        uint32_t node = free_head;
        free_head = reinterpret_cast<FreeNode*>(&buffer[node])->next_idx;
        return node;
    }

    [[gnu::always_inline]] inline void dealloc_raw(uint32_t idx) {
        if (idx == NULL_IDX || idx >= POOL_SIZE) return;
        reinterpret_cast<FreeNode*>(&buffer[idx])->next_idx = free_head;
        free_head = idx;
    }

    template <class... Args>
    [[gnu::always_inline]] inline uint32_t create(Args&&... args) {
        uint32_t idx = alloc_raw();
        if (idx == NULL_IDX) [[unlikely]] return NULL_IDX;
        new (&buffer[idx]) T(std::forward<Args>(args)...);
        return idx;
    }

    [[gnu::always_inline]] inline void destroy(uint32_t idx) {
        if (idx == NULL_IDX || idx >= POOL_SIZE) return;
        buffer[idx].~T();
        dealloc_raw(idx);
    }

    [[gnu::always_inline]] inline T& operator[](size_t idx) { return buffer[idx]; }
    [[gnu::always_inline]] inline const T& operator[](size_t idx) const { return buffer[idx]; }
    [[gnu::always_inline]] inline T* get_base_ptr() { return buffer; }
};

enum class L3Side : uint8_t { Sell = 'S', Buy = 'B' };
enum class L3PacketType : uint8_t { 
    AddOrder     = 0, 
    ModifyOrder  = 1, 
    CancelOrder  = 2, 
    ExecuteOrder = 3 
};

struct alignas(8) L3MarketPacket {
    uint32_t     order_id;       
    int32_t      price;        
    uint32_t     qty;          
    L3PacketType type;         
    L3Side       side;         
    uint8_t      _pad[10];     
};

struct alignas(32) Order {
    uint64_t timestamp_ns;
    uint32_t id;
    uint32_t qty;
    int32_t  price;
    uint32_t prev_idx;
    uint32_t next_idx;
    L3Side   side;
    char     pad[3];

    Order() = default;
    Order(uint32_t id_, uint32_t qty_, int32_t price_, L3Side side_, uint64_t ts = 0) 
        : timestamp_ns(ts), id(id_), qty(qty_), price(price_), prev_idx(NULL_IDX), next_idx(NULL_IDX), side(side_) {}
};

struct Level {
    uint32_t head = NULL_IDX;
    uint32_t tail = NULL_IDX;
    uint32_t total_qty = 0;
    uint32_t order_count = 0;

    [[gnu::always_inline]] inline void push_back(Order* pool, uint32_t node_idx) {
        Order& ord = pool[node_idx];
        ord.next_idx = NULL_IDX;
        ord.prev_idx = tail;

        if (tail == NULL_IDX) {
            head = tail = node_idx;
        } else {
            pool[tail].next_idx = node_idx;
            tail = node_idx;
        }
        total_qty += ord.qty;
        ++order_count;
    }

    [[gnu::always_inline]] inline void erase(Order* pool, uint32_t node_idx) {
        Order& ord = pool[node_idx];
        if (ord.prev_idx != NULL_IDX) {
            pool[ord.prev_idx].next_idx = ord.next_idx;
        } else {
            head = ord.next_idx;
        }

        if (ord.next_idx != NULL_IDX) {
            pool[ord.next_idx].prev_idx = ord.prev_idx;
        } else {
            tail = ord.prev_idx;
        }

        total_qty -= ord.qty;
        --order_count;
        ord.prev_idx = NULL_IDX;
        ord.next_idx = NULL_IDX;
    }

    [[gnu::always_inline]] inline uint32_t pop_front(Order* pool) {
        if (head == NULL_IDX) return NULL_IDX;
        uint32_t old_head = head;
        erase(pool, old_head);
        return old_head;
    }

    [[gnu::always_inline]] inline bool empty() const { return head == NULL_IDX; }
};

template<size_t MaxID = DIRECT_MAP_CAPACITY>
class OrderDirectMap {
private:
    alignas(64) uint32_t table[MaxID];

public:
    OrderDirectMap() {
        std::memset(table, 0xFF, sizeof(table));
    }

    [[gnu::always_inline]] inline void insert(uint32_t order_id, uint32_t pool_idx) {
        if (order_id < MaxID) [[likely]] {
            table[order_id] = pool_idx;
        }
    }

    [[gnu::always_inline]] inline uint32_t find(uint32_t order_id) const {
        if (order_id < MaxID) [[likely]] {
            return table[order_id];
        }
        return NULL_IDX;
    }

    [[gnu::always_inline]] inline void erase(uint32_t order_id) {
        if (order_id < MaxID) [[likely]] {
            table[order_id] = NULL_IDX;
        }
    }
};

struct QueuePosition {
    uint64_t timestamp_ns = 0;
    uint32_t order_id = 0;
    int32_t  price = 0;
    uint32_t qty = 0;
    uint32_t queue_rank = 0;
    uint32_t volume_ahead = 0;
    uint32_t volume_behind = 0;
    uint32_t total_level_qty = 0;
    double   queue_depletion_risk = 0.0;
    bool     found = false;
};

struct L3OrderView {
    uint32_t order_id = 0;
    uint32_t qty = 0;
    uint64_t timestamp_ns = 0;
};

template<size_t MaxQueueDepth = 8>
struct L3LevelSnapshot {
    int32_t  price = 0;
    uint32_t total_qty = 0;
    uint32_t total_orders = 0;
    size_t   visible_orders = 0;
    std::array<L3OrderView, MaxQueueDepth> orders{};
};

template<size_t PriceLevels = 5, size_t OrdersPerLevel = 5>
struct L3TelemetrySnapshot {
    std::array<L3LevelSnapshot<OrdersPerLevel>, PriceLevels> bids{};
    std::array<L3LevelSnapshot<OrdersPerLevel>, PriceLevels> asks{};
    size_t bid_depth_count = 0;
    size_t ask_depth_count = 0;

    int32_t  best_bid = 0;
    int32_t  best_ask = 0;
    uint32_t best_bid_qty = 0;
    uint32_t best_ask_qty = 0;

    int32_t  last_traded_price = -1;
    uint32_t last_traded_qty   = 0;
    uint64_t total_volume      = 0;
    double   cumulative_turnover = 0.0;

    uint64_t total_adds = 0;
    uint64_t total_cancels = 0;
    uint64_t total_execs = 0;

    [[gnu::always_inline]] inline bool is_valid() const {
        return (bid_depth_count > 0) && (ask_depth_count > 0) && (best_ask >= best_bid);
    }

    [[gnu::always_inline]] inline int32_t spread() const { 
        return is_valid() ? (best_ask - best_bid) : -1; 
    }

    [[gnu::always_inline]] inline double mid_price() const { 
        return is_valid() ? (best_bid + best_ask) * 0.5 : 0.0; 
    }

    [[gnu::always_inline]] inline double micro_price() const {
        if (!is_valid()) return 0.0;
        uint64_t denom = static_cast<uint64_t>(best_bid_qty) + best_ask_qty;
        if (denom == 0) return mid_price();
        return (static_cast<double>(best_bid_qty) * best_ask + static_cast<double>(best_ask_qty) * best_bid) / denom;
    }

    [[gnu::always_inline]] inline double vwap() const {
        return (total_volume > 0) ? (cumulative_turnover / static_cast<double>(total_volume)) : 0.0;
    }

    [[gnu::always_inline]] inline double cancel_to_fill_ratio() const {
        return (total_execs > 0) ? (static_cast<double>(total_cancels) / total_execs) : 0.0;
    }
    
    [[gnu::always_inline]] inline double order_book_imbalance() const {
        uint64_t b_vol = 0, a_vol = 0;
        for (size_t i = 0; i < bid_depth_count; ++i) b_vol += bids[i].total_qty;
        for (size_t i = 0; i < ask_depth_count; ++i) a_vol += asks[i].total_qty;
        uint64_t denom = b_vol + a_vol;
        if (denom == 0) return 0.0;
        return static_cast<double>(static_cast<int64_t>(b_vol) - static_cast<int64_t>(a_vol)) / denom;
    }
};

template<size_t MaxOrders = DEFAULT_MAX_ORDERS, size_t MaxID = DIRECT_MAP_CAPACITY>
class L3OrderBook {
private:
    Pricemap<LEVELS> bid_mask;
    Pricemap<LEVELS> ask_mask;
    int best_bid_idx = -1;
    int best_ask_idx = -1;

    Level bids[LEVELS];
    Level asks[LEVELS];

    MemoryPool<Order, MaxOrders> order_pool;
    OrderDirectMap<MaxID>        index_map;

    int last_trade_price = -1;
    uint32_t last_trade_qty = 0;
    uint64_t total_traded_volume = 0;
    double total_traded_turnover = 0.0;

    uint64_t total_adds = 0;
    uint64_t total_cancels = 0;
    uint64_t total_execs = 0;

public:
    template<L3Side S>
    [[gnu::always_inline]] inline void add_order(uint32_t order_id, int32_t target_price, uint32_t qty, uint64_t ts = 0) {
        const unsigned p_idx = static_cast<unsigned>(target_price - MIN_PRICE);
        if (p_idx >= LEVELS || qty == 0) [[unlikely]] return;

        ++total_adds;
        uint32_t remaining_qty = qty;
        constexpr bool IsBuy = (S == L3Side::Buy);

        if constexpr (IsBuy) {
            while (remaining_qty > 0 && best_ask_idx != -1 && best_ask_idx <= static_cast<int>(p_idx)) {
                Level& ask_level = asks[best_ask_idx];
                const int match_price = price(best_ask_idx);

                while (remaining_qty > 0 && !ask_level.empty()) {
                    uint32_t maker_node_idx = ask_level.head;
                    Order& maker_ord = order_pool[maker_node_idx];

                    if (maker_ord.next_idx != NULL_IDX) {
                        __builtin_prefetch(&order_pool[maker_ord.next_idx], 1, 1);
                    }

                    uint32_t fill_qty = std::min(remaining_qty, maker_ord.qty);
                    on_trade_execution(match_price, fill_qty);
                    ++total_execs;

                    remaining_qty -= fill_qty;

                    if (fill_qty == maker_ord.qty) {
                        index_map.erase(maker_ord.id);
                        ask_level.pop_front(order_pool.get_base_ptr());
                        order_pool.destroy(maker_node_idx);
                    } else {
                        maker_ord.qty -= fill_qty;
                        ask_level.total_qty -= fill_qty;
                    }
                }

                if (ask_level.empty()) {
                    ask_mask.clear(best_ask_idx);
                    best_ask_idx = ask_mask.find_min();
                }
            }
        } else {
            while (remaining_qty > 0 && best_bid_idx != -1 && best_bid_idx >= static_cast<int>(p_idx)) {
                Level& bid_level = bids[best_bid_idx];
                const int match_price = price(best_bid_idx);

                while (remaining_qty > 0 && !bid_level.empty()) {
                    uint32_t maker_node_idx = bid_level.head;
                    Order& maker_ord = order_pool[maker_node_idx];

                    if (maker_ord.next_idx != NULL_IDX) {
                        __builtin_prefetch(&order_pool[maker_ord.next_idx], 1, 1);
                    }

                    uint32_t fill_qty = std::min(remaining_qty, maker_ord.qty);
                    on_trade_execution(match_price, fill_qty);
                    ++total_execs;

                    remaining_qty -= fill_qty;

                    if (fill_qty == maker_ord.qty) {
                        index_map.erase(maker_ord.id);
                        bid_level.pop_front(order_pool.get_base_ptr());
                        order_pool.destroy(maker_node_idx);
                    } else {
                        maker_ord.qty -= fill_qty;
                        bid_level.total_qty -= fill_qty;
                    }
                }

                if (bid_level.empty()) {
                    bid_mask.clear(best_bid_idx);
                    best_bid_idx = bid_mask.find_max();
                }
            }
        }

        if (remaining_qty > 0) {
            uint32_t node_idx = order_pool.create(order_id, remaining_qty, target_price, S, ts);
            if (node_idx == NULL_IDX) [[unlikely]] return;

            index_map.insert(order_id, node_idx);

            if constexpr (IsBuy) {
                bids[p_idx].push_back(order_pool.get_base_ptr(), node_idx);
                bid_mask.set(p_idx);
                best_bid_idx = std::max(best_bid_idx, static_cast<int>(p_idx));
            } else {
                asks[p_idx].push_back(order_pool.get_base_ptr(), node_idx);
                ask_mask.set(p_idx);
                if (best_ask_idx == -1 || static_cast<int>(p_idx) < best_ask_idx) {
                    best_ask_idx = static_cast<int>(p_idx);
                }
            }
        }
    }

    [[gnu::always_inline]] inline void cancel_order(uint32_t order_id) {
        uint32_t node_idx = index_map.find(order_id);
        if (node_idx == NULL_IDX) [[unlikely]] return;

        ++total_cancels;
        Order& ord = order_pool[node_idx];
        const int p_idx = idx(ord.price);

        if (ord.side == L3Side::Buy) {
            bids[p_idx].erase(order_pool.get_base_ptr(), node_idx);
            if (bids[p_idx].empty()) {
                bid_mask.clear(p_idx);
                if (p_idx == best_bid_idx) [[unlikely]] best_bid_idx = bid_mask.find_max();
            }
        } else {
            asks[p_idx].erase(order_pool.get_base_ptr(), node_idx);
            if (asks[p_idx].empty()) {
                ask_mask.clear(p_idx);
                if (p_idx == best_ask_idx) [[unlikely]] best_ask_idx = ask_mask.find_min();
            }
        }

        index_map.erase(order_id);
        order_pool.destroy(node_idx);
    }

    template<L3Side S>
    [[gnu::always_inline]] inline void modify_order(uint32_t order_id, uint32_t new_qty) {
        uint32_t node_idx = index_map.find(order_id);
        if (node_idx == NULL_IDX) [[unlikely]] return;

        Order& ord = order_pool[node_idx];
        const int p_idx = idx(ord.price);

        if (new_qty == 0) {
            cancel_order(order_id);
            return;
        }

        constexpr bool IsBuy = (S == L3Side::Buy);

        if (new_qty <= ord.qty) {
            uint32_t diff = ord.qty - new_qty;
            ord.qty = new_qty;
            if constexpr (IsBuy) bids[p_idx].total_qty -= diff;
            else asks[p_idx].total_qty -= diff;
        } else {
            int32_t pr = ord.price;
            uint64_t ts = ord.timestamp_ns;
            cancel_order(order_id);
            add_order<S>(order_id, pr, new_qty, ts);
        }
    }

    template<L3Side S>
    [[gnu::always_inline]] inline void execute_order(uint32_t order_id, uint32_t exec_qty) {
        uint32_t node_idx = index_map.find(order_id);
        if (node_idx == NULL_IDX) [[unlikely]] return;

        Order& ord = order_pool[node_idx];
        const uint32_t actual_fill = std::min(exec_qty, ord.qty);
        const int p_idx = idx(ord.price);

        on_trade_execution(ord.price, actual_fill);
        ++total_execs;

        if (actual_fill == ord.qty) {
            cancel_order(order_id);
        } else {
            ord.qty -= actual_fill;
            if constexpr (S == L3Side::Buy) bids[p_idx].total_qty -= actual_fill;
            else asks[p_idx].total_qty -= actual_fill;
        }
    }

    [[gnu::always_inline]] inline void on_trade_execution(int target_price, uint32_t qty) {
        last_trade_price = target_price;
        last_trade_qty = qty;
        total_traded_volume += qty;
        total_traded_turnover += (static_cast<double>(target_price) * qty);
    }

    [[gnu::always_inline]] inline void process_packet(const L3MarketPacket& packet, uint64_t ts = 0) {
        switch (packet.type) {
            case L3PacketType::AddOrder:
                if (packet.side == L3Side::Buy) add_order<L3Side::Buy>(packet.order_id, packet.price, packet.qty, ts);
                else add_order<L3Side::Sell>(packet.order_id, packet.price, packet.qty, ts);
                break;
            case L3PacketType::ModifyOrder:
                if (packet.side == L3Side::Buy) modify_order<L3Side::Buy>(packet.order_id, packet.qty);
                else modify_order<L3Side::Sell>(packet.order_id, packet.qty);
                break;
            case L3PacketType::CancelOrder:
                cancel_order(packet.order_id);
                break;
            case L3PacketType::ExecuteOrder:
                if (packet.side == L3Side::Buy) execute_order<L3Side::Buy>(packet.order_id, packet.qty);
                else execute_order<L3Side::Sell>(packet.order_id, packet.qty);
                break;
        }
    }

    inline QueuePosition get_order_queue_position(uint32_t order_id) const {
        QueuePosition pos;
        uint32_t node_idx = index_map.find(order_id);
        if (node_idx == NULL_IDX) return pos;

        const Order& target = order_pool[node_idx];
        pos.found = true;
        pos.order_id = target.id;
        pos.price = target.price;
        pos.qty = target.qty;
        pos.timestamp_ns = target.timestamp_ns;

        const int p_idx = idx(target.price);
        const Level& lvl = (target.side == L3Side::Buy) ? bids[p_idx] : asks[p_idx];
        pos.total_level_qty = lvl.total_qty;

        uint32_t curr = lvl.head;
        uint32_t rank = 0;
        uint32_t ahead = 0;
        bool seen = false;

        while (curr != NULL_IDX) {
            const Order& o = order_pool[curr];
            if (curr == node_idx) {
                pos.queue_rank = rank;
                pos.volume_ahead = ahead;
                seen = true;
            } else if (!seen) {
                ahead += o.qty;
                ++rank;
            } else {
                pos.volume_behind += o.qty;
            }
            curr = o.next_idx;
        }

        pos.queue_depletion_risk = (pos.total_level_qty > 0) 
            ? (static_cast<double>(pos.volume_ahead) / pos.total_level_qty) 
            : 0.0;

        return pos;
    }

    template<size_t PriceLevels = 5, size_t OrdersPerLevel = 5>
    inline L3TelemetrySnapshot<PriceLevels, OrdersPerLevel> get_telemetry_snapshot() const {
        L3TelemetrySnapshot<PriceLevels, OrdersPerLevel> snap;

        snap.last_traded_price = last_trade_price;
        snap.last_traded_qty = last_trade_qty;
        snap.total_volume = total_traded_volume;
        snap.cumulative_turnover = total_traded_turnover;
        snap.total_adds = total_adds;
        snap.total_cancels = total_cancels;
        snap.total_execs = total_execs;

        int curr_bid = best_bid_idx;
        size_t b_idx = 0;
        while (curr_bid != -1 && b_idx < PriceLevels) {
            const Level& lvl = bids[curr_bid];
            auto& l_snap = snap.bids[b_idx];
            l_snap.price = price(curr_bid);
            l_snap.total_qty = lvl.total_qty;
            l_snap.total_orders = lvl.order_count;

            uint32_t o_curr = lvl.head;
            size_t o_cnt = 0;
            while (o_curr != NULL_IDX && o_cnt < OrdersPerLevel) {
                const Order& o = order_pool[o_curr];
                l_snap.orders[o_cnt] = { o.id, o.qty, o.timestamp_ns };
                ++o_cnt;
                o_curr = o.next_idx;
            }
            l_snap.visible_orders = o_cnt;

            if (b_idx == 0) {
                snap.best_bid = l_snap.price;
                snap.best_bid_qty = l_snap.total_qty;
            }

            ++b_idx;
            curr_bid = bid_mask.find_prev(curr_bid);
        }
        snap.bid_depth_count = b_idx;

        int curr_ask = best_ask_idx;
        size_t a_idx = 0;
        while (curr_ask != -1 && a_idx < PriceLevels) {
            const Level& lvl = asks[curr_ask];
            auto& l_snap = snap.asks[a_idx];
            l_snap.price = price(curr_ask);
            l_snap.total_qty = lvl.total_qty;
            l_snap.total_orders = lvl.order_count;

            uint32_t o_curr = lvl.head;
            size_t o_cnt = 0;
            while (o_curr != NULL_IDX && o_cnt < OrdersPerLevel) {
                const Order& o = order_pool[o_curr];
                l_snap.orders[o_cnt] = { o.id, o.qty, o.timestamp_ns };
                ++o_cnt;
                o_curr = o.next_idx;
            }
            l_snap.visible_orders = o_cnt;

            if (a_idx == 0) {
                snap.best_ask = l_snap.price;
                snap.best_ask_qty = l_snap.total_qty;
            }

            ++a_idx;
            curr_ask = ask_mask.find_next(curr_ask);
        }
        snap.ask_depth_count = a_idx;

        return snap;
    }
};