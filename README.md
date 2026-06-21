# ApexEngine: Ultra-Low Latency C++20 SPSC Matching Engine Pipeline

[![Low-Latency Engine Build Verification](https://github.com/honeyphantom/apexengine-low-latency-orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/honeyphantom/apexengine-low-latency-orderbook/actions)
[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg?style=flat-square)](https://en.cppreference.com/w/cpp/20)
[![Architecture](https://img.shields.io/badge/Architecture-Lock--Free%20%7C%20Zero--Copy%20%7C%20Cache--Aligned-orange.svg?style=flat-square)]()

`ApexEngine` is a high-performance, multi-threaded market data processing and order matching framework engineered explicitly for sub-microsecond algorithmic trading systems. By bypassing traditional dynamic allocations, lock-based concurrency bottlenecks, and data-copying overheads, the architecture establishes a deterministic execution critical path optimized for modern microarchitecture cache topologies.

---

## ⚡ Empirical Performance Profiles & Benchmarks

### 1. End-to-End Multithreaded Pipeline Throughput
Tested via a hardware-isolated system execution utilizing a randomized multi-threaded simulation engine processing an invariant transaction bundle across independent network worker and execution nodes:
* **Total Market Order Stream volume:** 5,000,000 Network Packets
* **Hardware Packet Storage Footprint:** 24 Bytes / Live Node
* **Aggregated Multi-Thread Processing Duration:** 611,298 microseconds
* **Mean Latency per Transaction Ingestion:** **122.26 nanoseconds**

### 2. Isolated Core OrderBook Mutation Latency
Profiled over **145,742,548 iterations** utilizing Google Benchmark framework isolation hooks running under tight `-O3 -march=native` compilation optimizations:
* **`BM_OrderBook_AddLimit` Operation Cost:** **4.80 nanoseconds**
* **Throughput Rate:** ~208.33 Million price-time prioritization updates per second per core.

---

## 🏗️ Architectural Core Optimization Vectors

### 1. Zero-Copy Single-Producer Single-Consumer (SPSC) Queue
Traditional ring buffers introduce cache invalidation overhead via sequential item copying. `ZeroCopySPSC` eliminates this cycle entirely:
* **Direct In-Place Slots:** Exposes raw memory addresses directly to the incoming receiver thread via `get_write_slot()`. Network packet buffers parse data straight into ring boundaries, entirely omitting thread-to-thread copying layers.
* **Cache-Line Alignment:** Enforces strict 64-byte padding (`alignas(64)`) on atomic write and read tracking indices (`write_idx`, `read_idx`). This completely maps out **False Sharing**, ensuring individual processing cores update positions without invalidating neighboring L1/L2 data rows.
* **Acquire-Release Memory Order Barriers:** Bypasses sequential consistency locks by applying atomic sequencing primitives (`std::memory_order_relaxed`, `std::memory_order_release`, and `std::memory_order_acquire`) to ensure safe, ultra-low overhead inter-thread visibility.

### 2. $O(1)$ Bitmask Pricemaps
To maximize the lookup and traversal speeds of active pricing depth rows, the engine employs a compressed two-tiered `Pricemap` scheme:
* **Hardware Instruction Level Scanning:** Leverages 64-bit word segmentations, deploying native compiler intrinsics (`__builtin_clzll` / `__builtin_ctzll`) to translate price row state lookups into single-cycle CPU instructions.
* **Deterministic Traversals:** Replaces complex binary tree configurations (`std::map`) or bucket structures (`std::unordered_map`) with contiguous index arrays. This guarantees deterministic $O(1)$ Top-Of-Book (Best Bid / Best Ask) lookups.

### 3. Pre-Allocated, Non-Contended Object Memory Pools
Dynamic memory operations (`std::malloc` or `new`) introduce unpredictable kernel calls and memory fragmentation into execution loops:
* **Contiguous Allocation Maps:** Constructs a fixed array pool matching data size layouts on system startup. 
* **In-Line Lifecycles:** Allocates fresh tracking nodes via zero-overhead placement new transformations (`new (ptr) T(...)`). Destructors are manually evaluated upon execution clearing, passing freed references back to a internal node stack via a deterministic array pointer.

### 4. Direct Core Affinity Pinning
Circumvents operating system thread-scheduling overhead and cache line eviction penalty loops by binding execution threads directly to fixed processing nodes:
* **Mac/Linux Unified Layer:** Coordinates cross-platform system scheduling calls (`pthread_setaffinity_np` under Linux and `thread_policy_set` within macOS Mach subsystems).
* **Core Topology Mapping:** Allocates traffic generators, raw network packet handling, and the central execution engine onto isolated logical cores:
    * **Core 2:** Mock Exchange Ingestion Processing Engine
    * **Core 3:** High-Speed IO Buffer Interface
    * **Core 4:** Core Order Book Execution Routing Core

### 5. Compile-Time Metaprogramming and Concept Constraints
Uses C++20 concepts and template traits to enforce high-performance memory bounds at compile time, completely eliminating run-time structural validation costs:
* **`ValidMarketPacket` Definition:** Enforces packet constraints at compile time, guaranteeing that data types are trivially copyable, conform to standard memory layout layouts, fit within a 128-byte ceiling, and have an alignment threshold $\ge 4$ bytes.
* **Compile-Time Configuration Validation:** Employs `consteval` constructors on `StaticConfigManager` to perform duplicate detection and size verification of network feed schemas during compilation. This catches structural configuration mismatches prior to generation.

---

## 📁 Repository Blueprint

```text
├── .github/workflows/
│   └── ci.yml             # Continuous automated GitHub Build Verification action
├── apps/
│   └── main.cpp           # End-to-end high-throughput multi-threaded pipeline harness
├── benchmarks/
│   └── book_benchmark.cpp # Google Benchmark performance micro-profiling architecture
├── include/low_latency/
│   ├── concepts.hpp       # C++20 design requirements and system validation types
│   ├── engine.hpp         # Memory pool allocator, SPSC queue, and execution mapping structures
│   └── thread_utils.hpp   # System-level processor affinity mapping abstractions
├── tests/
│   └── orderbook_test.cpp # Robust verification matrix tracking data integrity and partial fills
├── .gitignore             # Shields workspace from tracking local build paths
└── CMakeLists.txt         # Automated production toolchain configuration file