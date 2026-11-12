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

static constexpr int MIN_PRICE = 900;
static constexpr int MAX_PRICE = 1100;
static constexpr int LEVELS = MAX_PRICE - MIN_PRICE + 1;
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

enum class L1PacketType : uint8_t { SetLevel = 0, DeltaLevel = 1, CancelLevel = 2, TradePrint = 3 };
enum class L1Side : uint8_t { Sell = 'S', Buy = 'B' };

struct alignas(8) L1MarketPacket {
    uint32_t  order_id;       
    int32_t   price;        
    uint32_t  qty;          
    L1PacketType type;         
    L1Side    side;         
    uint8_t   _pad[10];     
};

struct L1Snapshot {
    int32_t  best_bid_price = -1;
    uint32_t best_bid_qty   = 0;
    int32_t  best_ask_price = -1;
    uint32_t best_ask_qty   = 0;

    int32_t  last_traded_price = -1;
    uint32_t last_traded_qty   = 0;
    uint64_t total_volume     = 0;
    double   cumulative_turnover = 0.0;

    [[gnu::always_inline]] inline bool is_valid() const {
        return (best_bid_price > 0) && (best_ask_price > 0) && (best_ask_price >= best_bid_price);
    }

    [[gnu::always_inline]] inline int32_t spread() const {
        return is_valid() ? (best_ask_price - best_bid_price) : -1;
    }

    [[gnu::always_inline]] inline double mid_price() const {
        return is_valid() ? (best_bid_price + best_ask_price) * 0.5 : 0.0;
    }

    [[gnu::always_inline]] inline double micro_price() const {
        const uint64_t sum_qty = static_cast<uint64_t>(best_bid_qty) + best_ask_qty;
        if (!is_valid() || sum_qty == 0) [[unlikely]] return mid_price();
        return (static_cast<double>(best_bid_qty) * best_ask_price +
                static_cast<double>(best_ask_qty) * best_bid_price) / static_cast<double>(sum_qty);
    }

    [[gnu::always_inline]] inline double imbalance() const {
        const uint64_t sum_qty = static_cast<uint64_t>(best_bid_qty) + best_ask_qty;
        if (sum_qty == 0) [[unlikely]] return 0.0;
        const int64_t diff = static_cast<int64_t>(best_bid_qty) - static_cast<int64_t>(best_ask_qty);
        return static_cast<double>(diff) / static_cast<double>(sum_qty);
    }

    [[gnu::always_inline]] inline double session_vwap() const {
        return (total_volume > 0) ? (cumulative_turnover / static_cast<double>(total_volume)) : 0.0;
    }
};

class L1OrderBook {
    Pricemap<LEVELS> bid_mask;
    Pricemap<LEVELS> ask_mask;
    int best_bid_idx = -1;
    int best_ask_idx = -1;
    uint32_t bids_qty[LEVELS]{0};
    uint32_t asks_qty[LEVELS]{0};

    int last_trade_price = -1;
    uint32_t last_trade_qty = 0;
    uint64_t total_traded_volume = 0;
    double total_traded_turnover = 0.0;
    template<L1Side S>
    [[gnu::always_inline]] void apply_set_level(int target_price, uint32_t qty) {
        const unsigned u_idx = static_cast<unsigned>(target_price - MIN_PRICE);
        if (u_idx >= LEVELS) [[unlikely]] return;
        const int p_idx = static_cast<int>(u_idx);
        constexpr bool IsBuy = (S == L1Side::Buy);
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
    template<L1Side S>
    [[gnu::always_inline]] void delta_level(int target_price, uint32_t qty) {
        const unsigned p_idx = static_cast<unsigned>(target_price - MIN_PRICE);
        if (p_idx >= LEVELS || qty == 0) [[unlikely]] return;
        constexpr bool IsBuy = (S == L1Side::Buy);
        uint32_t* side_arr = (IsBuy ? bids_qty : asks_qty);

        const int64_t current = side_arr[p_idx];
        const int64_t updated = current + qty;

        apply_set_level<S>(target_price, updated > 0 ? static_cast<uint32_t>(updated) : 0);
    }
public:
    template<L1Side S>
    [[gnu::always_inline]] void update_level(int target_price, uint32_t qty) {
        apply_set_level<S>(target_price, qty);
    }

    template<L1Side S>
    [[gnu::always_inline]] void delta_level(int target_price, int32_t delta_qty) {
        const unsigned p_idx = static_cast<unsigned>(target_price - MIN_PRICE);
        if (p_idx >= LEVELS || delta_qty == 0) [[unlikely]] return;
        constexpr bool IsBuy = (S == L1Side::Buy);
        uint32_t* side_arr = (IsBuy ? bids_qty : asks_qty);

        const int64_t current = side_arr[p_idx];
        const int64_t updated = current + delta_qty;

        apply_set_level<S>(target_price, updated > 0 ? static_cast<uint32_t>(updated) : 0);
    }

    template<L1Side S>
    [[gnu::always_inline]] void cancel_level(int price) {
        update_level<S>(price, 0);
    }

    [[gnu::always_inline]] void on_trade_execution(int target_price, uint32_t qty) {
        last_trade_price = target_price;
        last_trade_qty = qty;
        total_traded_volume += qty;
        total_traded_turnover += (static_cast<double>(target_price) * qty);
    }

    void execute_market_order(L1Side side, uint32_t match_qty) {
        uint32_t remaining = match_qty;

        if (side == L1Side::Buy) {
            while (remaining > 0 && best_ask_idx != -1) {
                const uint32_t top_qty = asks_qty[best_ask_idx];
                const int match_price = price(best_ask_idx);
                const uint32_t filled = std::min(remaining, top_qty);

                on_trade_execution(match_price, filled);
                delta_level<L1Side::Sell>(match_price, -static_cast<int32_t>(filled));
                remaining -= filled;
            }
        } else {
            while (remaining > 0 && best_bid_idx != -1) {
                const uint32_t top_qty = bids_qty[best_bid_idx];
                const int match_price = price(best_bid_idx);
                const uint32_t filled = std::min(remaining, top_qty);

                on_trade_execution(match_price, filled);
                delta_level<L1Side::Buy>(match_price, -static_cast<int32_t>(filled));
                remaining -= filled;
            }
        }
    }

    inline void process_packet(const L1MarketPacket& packet) {
        switch (packet.type) {
            case L1PacketType::SetLevel:
                if (packet.side == L1Side::Buy) update_level<L1Side::Buy>(packet.price, packet.qty);
                else update_level<L1Side::Sell>(packet.price, packet.qty);
                break;
            case L1PacketType::DeltaLevel:
                if (packet.side == L1Side::Buy) delta_level<L1Side::Buy>(packet.price, static_cast<int32_t>(packet.qty));
                else delta_level<L1Side::Sell>(packet.price, static_cast<int32_t>(packet.qty));
                break;
            case L1PacketType::CancelLevel:
                if (packet.side == L1Side::Buy) cancel_level<L1Side::Buy>(packet.price);
                else cancel_level<L1Side::Sell>(packet.price);
                break;
            case L1PacketType::TradePrint:
                on_trade_execution(packet.price, static_cast<uint32_t>(packet.qty));
                break;
        }
    }

    inline L1Snapshot get_snapshot() const {
        L1Snapshot snap;
        if (best_bid_idx != -1) {
            snap.best_bid_price = price(best_bid_idx);
            snap.best_bid_qty = bids_qty[best_bid_idx];
        }
        if (best_ask_idx != -1) {
            snap.best_ask_price = price(best_ask_idx);
            snap.best_ask_qty = asks_qty[best_ask_idx];
        }
        snap.last_traded_price = last_trade_price;
        snap.last_traded_qty = last_trade_qty;
        snap.total_volume = total_traded_volume;
        snap.cumulative_turnover = total_traded_turnover;
        return snap;
    }
};