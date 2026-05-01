## SUMMARY
The system is designed as a time-constrained asynchronous pipeline using SPSC (Single Producer Single Consumer) queues for inter-block communication. Each block operates independently while maintaining strict data ordering and bounded latency (≤ T).

## Executive Summary

A fully functional two-stage pipeline (Data Generation + Filter & Threshold) is implemented in C++17 with both threaded and single-threaded variants. The architecture prioritises modularity, testability, and extensibility for future  pipeline stages over raw throughput — a deliberate tradeoff documented in PERFORMANCE_ANALYSIS.md. The pipeline meets all functional requirements and the memory constraint (queue depth ≤ m). Throughput at T=1000ns is limited by OS scheduling non-determinism and virtual dispatch overhead inherent to the extensible design; the specific bottlenecks and the path to resolving them are analysed in detail in PERFORMANCE_ANALYSIS.md.

## DATA FLOW DIAGRAM
```
                ┌──────────────────────┐
                │     Config Loader    │
                │ (m, T, TV, kernel)   │
                └──────────┬───────────┘
                           │
                           ▼
                ┌──────────────────────┐
                │    Mode Controller   │
                │ (Random / CSV mode)  │
                └──────────┬───────────┘
                           │
        ┌──────────────────┴──────────────────┐
        │                                     │
        ▼                                     ▼
┌──────────────────┐              ┌────────────────────┐
│ Random Generator │              │   CSV Parser       │
└────────┬─────────┘              └────────┬───────────┘
         │                                 │
         └──────────────┬──────────────────┘
                        ▼
              ┌──────────────────────┐
              │  Generator Block     │
              │ (2 elements / T)     │
              └────────┬─────────────┘
                       │
                       ▼
            ┌──────────────────────────┐
            │   SPSC Lock-Free Queue   │
            │ (DataPacket stream)      │
            └────────┬─────────────────┘
                     │
                     ▼
            ┌──────────────────────────┐
            │   Filter + Threshold     │
            │  - Sliding window (9)    │
            │  - Padding handling      │
            │  - Dot product           │
            └────────┬─────────────────┘
                     │
                     ▼
            ┌──────────────────────────┐
            │   Output Writer          │
            │ (Buffered file / stdout) │
            └──────────────────────────┘
```

## Config Manager 
- **Responsibilities**: 

    - Load and validate configuration parameters (m, T, TV, kernel) from a file or command-line arguments. 

    - Provide an interface for other blocks to access these parameters.
- **Key Design Decisions**
- **Immutability:** Once created, SystemConfig is read-only
- **Explicit Dependency Injection:** Configuration is passed to each block during initialization
No Global State: Avoids hidden dependencies and improves testability
Extensibility: New parameters can be added without affecting existing components
 
## GeneratorBlock
- **Responsibilities**: 
    - Generate random data points based on the configuration parameters.
    - Ensure data is produced at the specified rate (2 elements / T).
    - Push generated data into the SPSC queue for further processing.

- **Modes**
    - **Random Mode**: Generates random data points.
    - **CSV Mode**: Reads data points from a CSV file.

## SPSC Lock-Free Queue
- **Responsibilities**: Lock free communication between the GeneratorBlock and the Filter + Threshold block.
- **Design**: Ring Buffer and Cache friendly design to minimize latency and maximize throughput.

- **Key Constraints**
    - Single producer → Generator
    - Single consumer → Filter

## Filter & Threshold Block (Most critical block)
- **Responsibilities**: 
    - Implement a sliding window of size 9 to process incoming data points.
    - Handle padding for the initial windows where there are fewer than 9 data points.
    - Compute the dot product of the current window with the kernel vector.
    - Compare the result against the threshold value (TV) and determine if it meets the criteria for output.

- **Internal Components**
    - **a) Sliding Window Buffer**
        -   Size = kernel size (default 9)
        -  Ring buffer (NO shifting)
    - **b) Padding Logic** [Preserve the order of data points]
        -  First 4 elements → left pad
        -  Last 4 elements → right pad