#pragma once

#include <iostream>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>
#include <array>
#include <span>
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
        if (!mask_level1[l1_idx]) mask_level2 &= ~(1ULL << l1_idx);
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

    inline int find_prev(int current_idx) const {
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

    inline int find_next(int current_idx) const {
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

enum class L2PacketType : uint8_t { SetLevel = 0, DeltaLevel = 1, CancelLevel = 2, TradePrint = 3 };
enum class L2Side : uint8_t { Sell = 'S', Buy = 'B' };

struct alignas(8) L2MarketPacket {
    uint32_t     order_id;       
    int32_t      price;        
    uint32_t     qty;          
    L2PacketType type;         
    L2Side       side;         
    uint8_t      _pad[10];     
};

struct PriceLevel {
    int32_t  price = 0;
    uint32_t qty   = 0;
};

template<size_t Depth = 10>
struct L2Snapshot {
    std::array<PriceLevel, Depth> bids{};
    std::array<PriceLevel, Depth> asks{};
    size_t bid_depth_count = 0;
    size_t ask_depth_count = 0;

    int32_t  last_traded_price = -1;
    uint32_t last_traded_qty   = 0;
    uint64_t total_volume      = 0;
    double   cumulative_turnover = 0.0;

    [[gnu::always_inline]] inline bool is_valid() const {
        return (bid_depth_count > 0) && (ask_depth_count > 0) && (asks[0].price >= bids[0].price);
    }

    [[gnu::always_inline]] inline int32_t spread() const {
        return is_valid() ? (asks[0].price - bids[0].price) : -1;
    }

    [[gnu::always_inline]] inline double mid_price() const {
        return is_valid() ? (bids[0].price + asks[0].price) * 0.5 : 0.0;
    }

    [[gnu::always_inline]] inline double micro_price() const {
        if (!is_valid()) return 0.0;
        const uint64_t sum_qty = static_cast<uint64_t>(bids[0].qty) + asks[0].qty;
        if (sum_qty == 0) [[unlikely]] return mid_price();
        return (static_cast<double>(bids[0].qty) * asks[0].price + static_cast<double>(asks[0].qty) * bids[0].price) / static_cast<double>(sum_qty);
    }

    [[gnu::always_inline]] inline double multi_level_imbalance() const {
        uint64_t total_bid_qty = 0;
        uint64_t total_ask_qty = 0;
        for (size_t i = 0; i < bid_depth_count; ++i) total_bid_qty += bids[i].qty;
        for (size_t i = 0; i < ask_depth_count; ++i) total_ask_qty += asks[i].qty;
        const uint64_t denom = total_bid_qty + total_ask_qty;
        if (denom == 0) return 0.0;
        return static_cast<double>(static_cast<int64_t>(total_bid_qty) - static_cast<int64_t>(total_ask_qty)) / static_cast<double>(denom);
    }

    [[gnu::always_inline]] inline double session_vwap() const {
        return (total_volume > 0) ? (cumulative_turnover / static_cast<double>(total_volume)) : 0.0;
    }
};

class L2OrderBook {
private:
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

    template<L2Side S>
    [[gnu::always_inline]] void apply_set_level(int p_idx, uint32_t qty) {
        constexpr bool IsBuy = (S == L2Side::Buy);
        uint32_t* side = (IsBuy ? bids_qty : asks_qty);
        auto& mask = (IsBuy ? bid_mask : ask_mask);

        side[p_idx] = qty;
        if (qty > 0) {
            mask.set(p_idx);
            if constexpr (IsBuy) {
                if (best_ask_idx != -1 && best_ask_idx <= p_idx) {
                    ask_mask.clear_less(p_idx);
                    std::memset(&asks_qty[best_ask_idx], 0, (p_idx - best_ask_idx + 1) * sizeof(uint32_t));
                    best_ask_idx = ask_mask.find_min();
                }
                best_bid_idx = std::max(best_bid_idx, p_idx);
            } else {
                if (best_bid_idx != -1 && best_bid_idx >= p_idx) {
                    bid_mask.clear_great(p_idx);
                    std::memset(&bids_qty[p_idx], 0, (best_bid_idx - p_idx + 1) * sizeof(uint32_t));
                    best_bid_idx = bid_mask.find_max();
                }
                if (best_ask_idx == -1 || p_idx < best_ask_idx) best_ask_idx = p_idx;
            }
        } else {
            mask.clear(p_idx);
            if constexpr (IsBuy) {
                if (p_idx == best_bid_idx) [[unlikely]] best_bid_idx = bid_mask.find_max();
            } else {
                if (p_idx == best_ask_idx) [[unlikely]] best_ask_idx = ask_mask.find_min();
            }
        }
    }

public:
template<L2Side S>
    [[gnu::always_inline]] void update_level(int target_price, uint32_t qty) {
        const unsigned p_idx = static_cast<unsigned>(target_price - MIN_PRICE);
        if (p_idx >= LEVELS) [[unlikely]] return;
        apply_set_level<S>(static_cast<int>(p_idx), qty);
    }

    template<L2Side S>
    [[gnu::always_inline]] void cancel_level(int target_price) {
        const unsigned p_idx = static_cast<unsigned>(target_price - MIN_PRICE);
        if (p_idx >= LEVELS) [[unlikely]] return;
        apply_set_level<S>(static_cast<int>(p_idx), 0);
    }   

    template<L2Side S>
    [[gnu::always_inline]] void delta_level(int target_price, int32_t delta_qty) {
        const unsigned p_idx = static_cast<unsigned>(target_price - MIN_PRICE);
        if (p_idx >= LEVELS || delta_qty == 0) [[unlikely]] return;
        constexpr bool IsBuy = (S == L2Side::Buy);
        uint32_t* side = (IsBuy ? bids_qty : asks_qty);

        const int64_t current = side[p_idx];
        const int64_t updated = current + delta_qty;
        
        if (current > 0 && updated > 0) [[likely]] {
            side[p_idx] = static_cast<uint32_t>(updated);
            return;
        }

        apply_set_level<S>(p_idx, updated > 0 ? static_cast<uint32_t>(updated) : 0);
    }

    [[gnu::always_inline]] void on_trade_execution(int target_price, uint32_t qty) {
        last_trade_price = target_price;
        last_trade_qty = qty;
        total_traded_volume += qty;
        total_traded_turnover += (static_cast<double>(target_price) * qty);
    }

    void execute_market_order(L2Side side, uint32_t match_qty) {
        uint32_t remaining = match_qty;

        if (side == L2Side::Buy) {
            while (remaining > 0 && best_ask_idx != -1) {
                const uint32_t top_qty = asks_qty[best_ask_idx];
                const int match_price = price(best_ask_idx);
                const uint32_t filled = std::min(remaining, top_qty);

                on_trade_execution(match_price, filled);
                delta_level<L2Side::Sell>(match_price, -static_cast<int32_t>(filled));
                remaining -= filled;
            }
        } else {
            while (remaining > 0 && best_bid_idx != -1) {
                const uint32_t top_qty = bids_qty[best_bid_idx];
                const int match_price = price(best_bid_idx);
                const uint32_t filled = std::min(remaining, top_qty);

                on_trade_execution(match_price, filled);
                delta_level<L2Side::Buy>(match_price, -static_cast<int32_t>(filled));
                remaining -= filled;
            }
        }
    }

    [[gnu::always_inline]] inline void process_packet(const L2MarketPacket& packet) {
        switch (packet.type) {
            case L2PacketType::SetLevel:
                if (packet.side == L2Side::Buy) update_level<L2Side::Buy>(packet.price, packet.qty);
                else update_level<L2Side::Sell>(packet.price, packet.qty);
                break;
            case L2PacketType::DeltaLevel:
                if (packet.side == L2Side::Buy) delta_level<L2Side::Buy>(packet.price, static_cast<int32_t>(packet.qty));
                else delta_level<L2Side::Sell>(packet.price, static_cast<int32_t>(packet.qty));
                break;
            case L2PacketType::CancelLevel:
                if (packet.side == L2Side::Buy) cancel_level<L2Side::Buy>(packet.price);
                else cancel_level<L2Side::Sell>(packet.price);
                break;
            case L2PacketType::TradePrint:
                on_trade_execution(packet.price, packet.qty);
                break;
        }
    }

    template<size_t Depth = 10>
    inline L2Snapshot<Depth> get_snapshot() const {
        L2Snapshot<Depth> snap;

        int curr_bid = best_bid_idx;
        size_t b_cnt = 0;
        while (curr_bid != -1 && b_cnt < Depth) {
            snap.bids[b_cnt] = { price(curr_bid), bids_qty[curr_bid] };
            ++b_cnt;
            curr_bid = bid_mask.find_prev(curr_bid);
        }
        snap.bid_depth_count = b_cnt;

        int curr_ask = best_ask_idx;
        size_t a_cnt = 0;
        while (curr_ask != -1 && a_cnt < Depth) {
            snap.asks[a_cnt] = { price(curr_ask), asks_qty[curr_ask] };
            ++a_cnt;
            curr_ask = ask_mask.find_next(curr_ask);
        }
        snap.ask_depth_count = a_cnt;

        snap.last_traded_price = last_trade_price;
        snap.last_traded_qty = last_trade_qty;
        snap.total_volume = total_traded_volume;
        snap.cumulative_turnover = total_traded_turnover;
        return snap;
    }
};