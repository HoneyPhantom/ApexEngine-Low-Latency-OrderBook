#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(__ARM_NEON)
    #include <arm_neon.h>
    #define SIMD_W 4
    typedef uint32x4_t simd_reg;
    #define LOAD_SIMD(ptr) vld1q_u32(ptr)
    #define STORE_SIMD(ptr, val) vst1q_u32(ptr, val)
    #define MAC_SIMD(acc, mul1, mul2) vmlaq_u32(acc, mul1, mul2)
    #define SET1_SIMD(val) vdupq_n_u32(val)
#elif defined(__AVX512F__)
    #include <immintrin.h>
    #define SIMD_W 16
    typedef __m512i simd_reg;
    #define LOAD_SIMD(ptr) _mm512_loadu_si512(ptr)
    #define STORE_SIMD(ptr, val) _mm512_storeu_si512(ptr, val)
    #define MAC_SIMD(acc, mul1, mul2) _mm512_add_epi32(acc, _mm512_mullo_epi32(mul1, mul2))
    #define SET1_SIMD(val) _mm512_set1_epi32(val)
#endif

#include "low_latency/concepts.hpp"
#include "low_latency/engine.hpp"
#include "low_latency/thread_utils.hpp"

using ActivePacketType = CME_FuturePacket; 
constexpr std::string_view ACTIVE_STREAM_ID = "CME_FUTURES";

constexpr std::array<SchemaConfig, 2> network_grid{{
    { "NYSE_EQUITIES", 16, FeedProtocol::UDP_MC,     4096 },
    { "CME_FUTURES",   24, FeedProtocol::DIRECT_DMA, 8192 }
}};
constexpr StaticConfigManager config_manager(network_grid);
static_assert(config_manager.validate_packet_compatibility<ActivePacketType>(ACTIVE_STREAM_ID), "Hardware configuration map variant sizing error!");
              
OrderBook book;
ZeroCopySPSC<DataChunk<ActivePacketType>, QUEUE_CAPACITY> zero_copy_queue;
std::atomic<bool> is_receiver_ready{false};

template<ValidMarketPacket PacketType>
class MarketDataReceiver {
private:
    int server_fd = -1;
    ZeroCopySPSC<DataChunk<PacketType>, QUEUE_CAPACITY>& queue;
    int active_port = -1;

public:
    MarketDataReceiver(ZeroCopySPSC<DataChunk<PacketType>, QUEUE_CAPACITY>& q_ref, int base_port) : queue(q_ref) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (server_fd < 0) throw std::runtime_error("Socket creation failed");

        int flags = fcntl(server_fd, F_GETFL, 0);
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        int rcvbuf_size = 8 * 1024 * 1024;
        setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        bool bound_successfully = false;
        for (int offset = 0; offset < 4; ++offset) {
            int current_port = base_port + offset;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons(current_port);

            if (::bind(server_fd, (struct sockaddr*)&address, sizeof(address)) >= 0) {
                active_port = current_port;
                bound_successfully = true;
                break;
            }
        }

        if (!bound_successfully) {
            throw std::runtime_error("Socket binding failed: All ports in fallback range are locked.");
        }
    }

    ~MarketDataReceiver() { if (server_fd != -1) close(server_fd); }

    int get_active_port() const { return active_port; }

    void run_receiver_loop(size_t max_packets_to_process) {
        
        pin_thread_to_core(3);

        size_t packets_received = 0;
        DataChunk<PacketType>* current_slot = nullptr;
        while ((current_slot = queue.get_write_slot()) == nullptr) hardware_spin_relax();
        current_slot->valid_count = 0;

        std::cout << "Engine active. Running Zero-Copy In-Place SPSC Engine on Port " << active_port << "..." << std::endl;

        alignas(64) PacketType network_buffer[65536]; 
        is_receiver_ready.store(true, std::memory_order_release);

        while (packets_received < max_packets_to_process) {
            ssize_t bytes_read = recv(server_fd, network_buffer, sizeof(network_buffer), 0);
            if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; 
                break;
            }
            size_t packets_in_batch = static_cast<size_t>(bytes_read) / sizeof(PacketType);
            for (size_t i = 0; i < packets_in_batch; ++i) {
                current_slot->packets[current_slot->valid_count++] = network_buffer[i];
                ++packets_received;
                if (current_slot->valid_count == CHUNK_SIZE) {
                    queue.commit_write(); 
                    while ((current_slot = queue.get_write_slot()) == nullptr) hardware_spin_relax();
                    current_slot->valid_count = 0;
                }
            }
        }
        if (current_slot && current_slot->valid_count > 0) queue.commit_write();
        std::cout << "Target boundary satisfied. Processing halted." << std::endl;
    }
};

template<ValidMarketPacket PacketType>
void run_matching_engine_consumer(OrderBook& book_obj, ZeroCopySPSC<DataChunk<PacketType>, QUEUE_CAPACITY>& q_ref, size_t total_packets) {
    
    pin_thread_to_core(4);
    
    size_t processed = 0;
    while (processed < total_packets) {
        const DataChunk<PacketType>* current_slot = q_ref.peek_read_slot();
        if (current_slot != nullptr) {
            size_t count = current_slot->valid_count;
            for (size_t i = 0; i < count; ++i) {
                const auto& packet = current_slot->packets[i];
                book_obj.add_in_limit(packet.side == 'B', packet.price, packet.qty);
            }
            processed += count;
            q_ref.commit_read();
        } 
        else hardware_spin_relax();
    }
}

template<ValidMarketPacket PacketType>
void run_mock_exchange_tx(int port, size_t total_packets) {
    
    pin_thread_to_core(2);

    std::cout << "Mock Exchange Thread waiting for Receiver to initialize..." << std::endl;
    while (!is_receiver_ready.load(std::memory_order_acquire)) hardware_spin_relax();
    std::cout << "Receiver detected! Injecting packets into Port " << port << "..." << std::endl;

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    std::cout << "Mock Exchange Thread running on Core 2. Injecting " << total_packets << " packets into Port " << port << "..." << std::endl;

    const size_t packets_per_frame = 32; 
    std::vector<PacketType> frame_buffer(packets_per_frame);

    alignas(64) uint32_t rng[SIMD_W];
    for(int i = 0; i < SIMD_W; ++i) rng[i] = 42 + (i * 1000);

    simd_reg v_state = LOAD_SIMD(rng);
    simd_reg v_a = SET1_SIMD(1664525u);
    simd_reg v_c = SET1_SIMD(1013904223u);

    size_t packets_sent = 0;
    while (packets_sent < total_packets) {
        for (size_t i = 0; i < packets_per_frame; i += SIMD_W) {
            
            v_state = MAC_SIMD(v_state, v_a , v_c);
            
            alignas(64) uint32_t results[SIMD_W];
            STORE_SIMD(results, v_state);

            for(size_t j = 0; j < SIMD_W; ++j) {
                size_t idx = i + j;
                frame_buffer[idx].type = 'Q';
                frame_buffer[idx].order_id = static_cast<uint32_t>(packets_sent + idx + 1);
                frame_buffer[idx].qty = 1 + (results[j] % 20);
                frame_buffer[idx].price = MIN_PRICE + (results[j] % LEVELS);
                frame_buffer[idx].side = (results[j] & 1) ? 'B' : 'S';
            }
        }

        sendto(client_fd, frame_buffer.data(), packets_per_frame * sizeof(PacketType), 0,
               (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        packets_sent += packets_per_frame;
    }
    close(client_fd);
    std::cout << "Mock Exchange finished transmission." << std::endl;
}

int main() {
    const int BASE_PORT = 12345;
    const size_t TOTAL_PACKETS = 5000000;
    using clock = std::chrono::steady_clock;

    try {
        MarketDataReceiver<ActivePacketType> receiver(zero_copy_queue, BASE_PORT);
        int resolved_port = receiver.get_active_port();

        std::cout << "Starting Lock-Free ZERO-COPY SPSC Pipeline..." << std::endl;
        std::cout << "Mock Exchange Thread -> Core 2" << std::endl;
        std::cout << "IO Network Thread    -> Core 3" << std::endl;
        std::cout << "Matching Engine Core -> Core 4" << std::endl;

        std::thread exchange_thread(run_mock_exchange_tx<ActivePacketType>, resolved_port, TOTAL_PACKETS);
        
        auto start_time = clock::now();

        std::thread engine_thread(run_matching_engine_consumer<ActivePacketType>, std::ref(book), std::ref(zero_copy_queue), TOTAL_PACKETS);
        receiver.run_receiver_loop(TOTAL_PACKETS);
        
        if (engine_thread.joinable()) engine_thread.join();
        auto end_time = clock::now();
        
        if (exchange_thread.joinable()) exchange_thread.join();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

        std::cout << "\n================ ENGINE RUN STATS ================" << std::endl;
        std::cout << "Total Data Streamed    : " << TOTAL_PACKETS << " Packets" << std::endl;
        std::cout << "Packet Core Footprint  : " << sizeof(ActivePacketType) << " Bytes" << std::endl;
        std::cout << "Total Processing Time  : " << duration << " microseconds" << std::endl;
        std::cout << "Average Cost Per Packet: " << (double)duration * 1000.0 / TOTAL_PACKETS << " nanoseconds" << std::endl;
        std::cout << "Final Matching State   : Best Bid: " << book.get_best_bid() 
                  << " | Best Ask: " << book.get_best_ask() << std::endl;
        std::cout << "==================================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Engine Runtime Failure Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}