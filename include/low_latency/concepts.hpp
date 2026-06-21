#pragma once

#include <string_view>
#include <concepts>
#include <type_traits>
#include <array>
#include <stdexcept>

template<typename T>
concept ValidMarketPacket = 
    std::is_trivially_copyable_v<T> && 
    std::is_standard_layout_v<T> &&
    (sizeof(T) <= 128) && 
    (alignof(T) >= 4) && 
    requires {
        { T::type } -> std::convertible_to<char>;
        { T::price } -> std::convertible_to<int32_t>;
        { T::qty } -> std::convertible_to<uint32_t>;
    };

enum class FeedProtocol { UDP_MC, TCP_ITCH, DIRECT_DMA };

struct SchemaConfig {
    std::string_view stream_id;
    uint32_t expected_packet_size;
    FeedProtocol protocol;
    uint32_t max_burst_capacity;
};

template<size_t N>
class StaticConfigManager {
private:
    std::array<SchemaConfig, N> schemas;

    static consteval bool strings_equal(std::string_view a, std::string_view b) {
        return a == b;
    }

public:
    consteval StaticConfigManager(std::array<SchemaConfig, N> input_schemas) : schemas(input_schemas) {
        for (size_t i = 0; i < N; ++i) {
            if (schemas[i].stream_id.empty()) {
                throw std::logic_error("Configuration Error: Stream ID cannot be empty!");
            }
            if (schemas[i].max_burst_capacity < 64) {
                throw std::logic_error("Configuration Error: Burst capacity must be at least 64 packets!");
            }
            for (size_t j = i + 1; j < N; ++j) {
                if (strings_equal(schemas[i].stream_id, schemas[j].stream_id)) {
                    throw std::logic_error("Configuration Error: Duplicate Stream ID detected!");
                }
            }
        }
    }

    consteval SchemaConfig get_schema(std::string_view id) const {
        for (size_t i = 0; i < N; ++i) {
            if (schemas[i].stream_id == id) return schemas[i];
        }
        throw std::logic_error("Configuration Error: Requested Stream ID was not found!");
    }

    template<ValidMarketPacket PacketType>
    consteval bool validate_packet_compatibility(std::string_view id) const {
        SchemaConfig matching_schema = get_schema(id);
        if (sizeof(PacketType) != matching_schema.expected_packet_size) {
            throw std::logic_error("Validation Error: Hardware packet memory size mismatch!");
        }
        return true;
    }
};

#pragma pack(push, 1)
struct alignas(4) NYSE_EquityPacket {
    char type;          // 1 byte
    uint32_t order_id;  // 4 bytes
    uint32_t qty;       // 4 bytes
    int32_t price;      // 4 bytes
    char side;          // 1 byte
    char pad[2];        // 2 bytes
};                      // Total: 16 bytes
#pragma pack(pop)

#pragma pack(push, 1)
struct alignas(4) CME_FuturePacket {
    char type;          // 1 byte
    uint64_t timestamp; // 8 bytes
    uint32_t order_id;  // 4 bytes
    uint32_t qty;       // 4 bytes
    int32_t price;      // 4 bytes
    char side;          // 1 byte
    char pad[2];        // 2 bytes
};                      // Total: 24 bytes
#pragma pack(pop)