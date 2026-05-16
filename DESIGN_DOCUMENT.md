# CynLr Pipeline — Design Overview Document
**C++17 | Visual Studio 2017+ | Windows x64**

---

## Executive Summary

A fully functional four-stage pipeline (Data Generation → Filter & Threshold → Labelling → Tracing & Computation) is implemented in C++17, targeting Visual Studio 2017 and later on Windows x64. Two execution models are provided:

- **CynLr_multiThread** — four dedicated threads connected by lock-free SPSC queues. Each stage runs concurrently, pipelined, with strict ≤ T latency budgets.
- **CynLr_Linear** — single-threaded, queue-free baseline for pure algorithm cost measurement and profiling.

Both models share all core logic classes (SlidingWindow, LabelMap, RowLabelBuffer, BlobAccumulator) through common headers. The architecture prioritises modularity, testability, and extensibility across all four stages over raw throughput — a deliberate tradeoff documented in PERFORMANCE_ANALYSIS.md.

All functional requirements are met. Memory constraint (queue depth ≤ m/2) is satisfied across every stage. Throughput at tight T values is limited by Windows OS scheduling non-determinism, not by the algorithm itself; the specific bottlenecks and remediation paths are analysed in PERFORMANCE_ANALYSIS.md.

---

## 1. Architecture — What Pattern Fits This Problem?

### 1.1 Pipeline Pattern

The problem is structurally a **streaming pipeline with causal dependencies**:

```
Generator → [queue] → Filter → [queue] → Labeller → [queue] → Tracer → Output
```

Each stage is a self-contained processing unit. Stages do not share mutable state. All inter-stage communication happens exclusively through bounded queues. This maps directly to the **Pipeline architectural pattern** (a well-known concurrency pattern for streaming dataflows).

Within each stage, the internal design follows the **Strategy pattern** (e.g. `IDataSource` for CSV vs random mode, `IOutputWriter` for null vs CSV output, `ITracingOutput` for blob sinks) and **Factory Method** (createDataSource, makeOutputWriter, makeTracingOutput) for runtime polymorphic construction.

### 1.2 Why Not Shared Memory / Global State?

Every block receives its dependencies at construction time (**Dependency Injection**). `SystemConfig` is immutable after construction and passed by const-reference to each block. There is no global state except the signal handler flag (`g_shutdown_requested`). This makes each block independently unit-testable with no test-double scaffolding beyond constructing a `SimpleQueue`.

### 1.3 Why Two Execution Models?

The threaded model is the production path — it achieves pipeline overlap so stage N+1 processes packet K while stage N processes packet K+1. The linear model exists purely as a measurement baseline to separate algorithm cost from synchronisation cost. Results in PERFORMANCE_ANALYSIS.md confirm the threaded overhead is ~500 ns/pixel versus ~144 ns for pure algorithm.

---

## 2. Data Flow Diagram

```
                    ┌──────────────────────────┐
                    │      Config Loader       │
                    │  (m, T, TV, kernel,      │
                    │   mode, boundary policy) │
                    └────────────┬─────────────┘
                                 │ SystemConfig (immutable)
                                 │ injected into every block
                    ┌────────────▼─────────────┐
                    │     Mode Controller      │
                    │   (Random / CSV mode)    │
                    └────────────┬─────────────┘
                                 │
              ┌──────────────────┴─────────────────┐
              │                                    │
              ▼                                    ▼
   ┌────────────────────┐              ┌────────────────────┐
   │  RandomDataSource  │              │   CSVDataSource    │
   │  (IDataSource)     │              │   (IDataSource)    │
   └────────┬───────────┘              └────────┬───────────┘
            └──────────────┬───────────────────-┘
                           ▼
                 ┌──────────────────────┐
                 │    GeneratorBlock    │
                 │  (2 pixels / cycle T)│
                 └──────────┬───────────┘
                            │  DataPacket {v1, v2, row, col}
                            ▼
              ┌──────────────────────────────┐
              │  DynamicSPSCQueue<DataPacket>│
              │  depth = m/2, lock-free      │
              └──────────┬───────────────────┘
                         │
                         ▼
              ┌──────────────────────────────┐
              │       FilterBlock            │
              │  - SlidingWindow (9 slots)   │
              │  - BoundaryPolicy (replicate │
              │    or zero_pad)              │
              │  - 9-tap dot product         │
              │  - BinaryThresholder         │
              └──────────┬───────────────────┘
                         │  FilteredPacket {b1, b2, row, col}
                         ▼
           ┌────────────────────────────────────┐
           │ DynamicSPSCQueue<FilteredPacket>   │
           │ depth = m/2, lock-free             │
           └──────────┬─────────────────────────┘
                      │
                      ▼
           ┌────────────────────────────────────┐
           │         LabellingBlock             │
           │  - RowLabelBuffer (prev/curr rows) │
           │  - LabelMap (Union-Find, m/2 cap.) │
           │  - 4-causal neighbour labelling    │
           │  - merge & recycle event emission  │
           └──────────┬─────────────────────────┘
                      │  LabelledPacket {l1, l2, merge, recycle}
                      ▼
           ┌────────────────────────────────────┐
           │ DynamicSPSCQueue<LabelledPacket>   │
           │ depth = m/2, lock-free             │
           └──────────┬─────────────────────────┘
                      │
                      ▼
           ┌────────────────────────────────────┐
           │         TracingBlock               │
           │  - BlobAccumulator[] per label     │
           │  - local LabelMap (mirror)         │
           │  - merge/recycle event replay      │
           │  - pixel count + bounding box      │
           └──────────┬─────────────────────────┘
                      │  CompletedBlob {label, count, bbox}
                      ▼
           ┌────────────────────────────────────┐
           │       ITracingOutput               │
           │  NullTracingOutput / CSV / Stdout  │
           └────────────────────────────────────┘
```

---

## 3. Stage Descriptions

### 3.1 Config Manager

**Responsibilities:** Load, parse, and validate `SystemConfig` from `config.cfg` and CLI overrides. Provides a single immutable configuration struct to all blocks.

**Key Design Decisions:**
- **Immutability:** `SystemConfig` is read-only after `load()` returns. No block modifies it.
- **Explicit Dependency Injection:** Each block receives `const SystemConfig&` at construction. No block calls `ConfigManager` at runtime.
- **No Global State:** Avoids hidden couplings and enables parallel unit testing.
- **Extensibility:** New config keys are silently skipped by existing parsers — forward-compatible.

### 3.2 GeneratorBlock

**Responsibilities:** Produce `DataPacket` pairs at exactly one packet per cycle T. Supports two source modes via the `IDataSource` interface.

**Modes:**
- **RANDOM:** `RandomDataSource` — infinite Mersenne Twister stream. Supports deterministic `--seed=N` for reproducibility.
- **CSV:** `CSVDataSource` — finite stream from a `.csv` file. Auto-detects column count from the first row. Supports three mismatch policies (REJECT / TRUNCATE / ZERO_PAD).

**Timing strategy:** Sleep for `T - 20 µs`, then spin-wait the final margin using `_mm_pause` / `__builtin_ia32_pause` for sub-microsecond precision at the cycle boundary. Pure spin for T < 1 ms.

**Back-pressure:** If the downstream queue is full at deadline, the packet is counted as dropped and the cycle moves on — preserving the timing contract at the cost of one dropped packet. This is logged in the pipeline summary.

### 3.3 FilterBlock

**Responsibilities:** Apply a 9-tap convolution to each pixel using a sliding ring-buffer window, then threshold to binary.

**Internal Components:**
- `SlidingWindow` — ring buffer of `WindowSlot {value, row, col}`, capacity = kernel size. No shifting; O(1) push and read via head pointer + modulo.
- `BoundaryPolicy` — REPLICATE (extend edge pixel) or ZERO_PAD (fill with 0). Applied symmetrically at both ends of each scan row.
- `dotProduct()` — 9-tap case fully unrolled at compile time (no loop, no branch). Generic loop fallback for non-standard kernel sizes.
- `BinaryThresholder` — inline `>= threshold` comparison; no virtual dispatch on the hot path (replaced by direct float comparison for performance).
- `PendingOutput` — staging struct that pairs consecutive binary pixels into `FilteredPacket` before emitting.

**Row transition:** On every new row index, `flushRowEnd()` right-pads the previous row, then `window_.reset()` + left-pad begins the new row. The `FP_FLAG_B2_IS_PAD` flag marks synthetic padding pixels so downstream stages can skip them.

### 3.4 LabellingBlock

**Responsibilities:** Assign connected-component labels to binary pixels using an online 4-causal-neighbour scan. Emit merge and recycle events for downstream consumption.

**Algorithm:** Streaming Rosenfeld–Pfaltz connected-components with Union-Find (path-halving, union-by-rank). The four causal neighbours accessible at pixel (r, c) are NW, N, NE (from `prev_row[]`) and W (from `curr_row[]`). This is sufficient for 8-connected labelling in a raster scan.

**Key data structures:**
- `LabelMap` — bounded Union-Find over `uint16_t` label IDs in `[1, m/2]`. `find()` uses path-halving (iterative, O(α(n)) amortised). `unite()` always keeps the lower label as root (lower label survives, matching the spec example).
- `RowLabelBuffer` — double-buffered label arrays (`prev_[]` and `curr_[]`) with `uint8_t` presence flags. `commitAndRecycle()` swaps buffers at row boundaries. `drainDeadFromPrev()` frees label slots mid-row as soon as a COOLING label's last possible NE reach is passed — preventing LabelMap exhaustion on infinite streams.

**Memory constraint:** Maximum simultaneous active labels ≤ m/2 by construction. Proven by exhaustive case analysis over all 2-pixel binary patterns.

**Event protocol:**
- `merge_old` / `merge_new` — emitted when a pixel connects two previously separate components. Lower label survives.
- `recycled` — emitted when a label is permanently dead (absent from both prev and curr rows). At most one recycle event per packet; extras are queued in `pending_recycles_` and spread across subsequent packets.

### 3.5 TracingBlock

**Responsibilities:** Accumulate per-label pixel statistics (count, bounding box) by replaying merge and recycle events from `LabelledPacket`. Emit `CompletedBlob` records when a label is recycled.

**Why a local LabelMap?** LabellingBlock and TracingBlock run on separate threads. Sharing one LabelMap would require a mutex on every `find()` / `unite()` call — a serialisation point on the hot path. Instead, TracingBlock maintains its own Union-Find and keeps it consistent with LabellingBlock's map by replaying the merge events from `LabelledPacket` in SPSC-queue order. This is correct because SPSC ordering guarantees TracingBlock sees events in the same sequence LabellingBlock produced them.

**Accumulator array:** `BlobAccumulator[max_labels + 1]` flat vector, indexed by canonical label ID. For m=130: 66 × ~48 bytes ≈ 3.2 KB — L1-resident. No heap allocation on the per-packet hot path.

**Flush on exit:** After `run()` drains the input queue, any still-active accumulators (blobs that reached end-of-stream without a recycle event) are emitted before `output_.flush()` is called. This ensures CSV mode produces a complete blob list even when the input ends without blank trailing rows.

---

## 4. Communication Mechanism Between Blocks

### 4.1 Production Path: DynamicSPSCQueue

All three inter-block hot-path channels use `DynamicSPSCQueue<T>`:

- **Lock-free:** No mutex, no CAS on the data path. Uses acquire/release atomics on `head_` and `tail_` (separate cache lines — `alignas(64)` — to eliminate false sharing).
- **Ring buffer:** Power-of-two capacity, index masking (AND) instead of modulo. Heap-allocated once at construction.
- **Runtime capacity:** Sized from `m/2` at runtime so queue depth scales with scan width without recompiling.
- **Logical capacity limit:** A second softer limit enforces the spec's "queue depth ≤ m" memory constraint. `push()` returns false when occupancy exceeds this limit, triggering back-pressure.
- **Peak tracking:** `peak_occupancy_` atomic high-water mark, updated via CAS on every push. Reported in the pipeline summary.

Queue depths for all three inter-stage channels:

| Channel | Type | Depth |
|---|---|---|
| Generator → Filter | `DynamicSPSCQueue<DataPacket>` | m/2 |
| Filter → Labeller | `DynamicSPSCQueue<FilteredPacket>` | m/2 |
| Labeller → Tracer | `DynamicSPSCQueue<LabelledPacket>` | m/2 |

### 4.2 Test / Drain Path: SimpleQueue

`SimpleQueue<T>` — mutex-protected `std::queue<T>`, unbounded. Used in unit tests and for draining after pipeline shutdown. Not suitable for the hot path (mutex cost ~20–50 ns violates the <100 ns per-pixel budget at tight T values).

### 4.3 IQueue<T> Interface

All blocks accept `IQueue<T>&` at construction. This decouples blocks from the queue implementation and allows unit tests to substitute `SimpleQueue` for `DynamicSPSCQueue` without changing any block code — the Strategy pattern applied to the communication channel.

---

## 5. Scalability — Adding Future Blocks

The pipeline was designed with future stages (Labelling Block 3 and Tracing Block 4, both now implemented) in mind. Adding a fifth or sixth stage requires:

1. Define the new packet struct in a `NewStageUtils.hpp` (following the FilterUtils / LabellingUtils / TracingUtils pattern).
2. Implement `NewStageBlock` with constructor `(SystemConfig&, IQueue<PrevPacket>&, IQueue<NewPacket>&)` and the `run()` / `stop()` interface.
3. In `main.cpp`: declare one new `DynamicSPSCQueue`, construct the block, launch a thread, add it to the drain-and-stop sequence.
4. In `src/CMakeLists.txt`: add `NewStageBlock.cpp` to `CYNLR_SOURCES`.

No existing block, queue, or config code changes. The `SystemConfig` struct silently ignores unknown keys, so new configuration parameters are forward-compatible with old config files.

---

## 6. Modularity — Agnosticism to Changes

Each block is isolated from changes in adjacent blocks by the packet interface and the `IQueue<T>` abstraction:

| Change | Affected files | Unaffected files |
|---|---|---|
| Replace RNG algorithm | `GeneratorBlock.cpp` | All downstream blocks |
| Change kernel size / type | `ConfigManager.cpp`, `config.cfg` | Generator, Labeller, Tracer |
| Switch boundary policy | `config.cfg` (runtime) | All other files |
| Replace SPSC with shared-memory channel | `Queue.hpp` (new impl), `main.cpp` | All blocks |
| Add a fifth pipeline stage | New `.hpp/.cpp`, `main.cpp` | All existing blocks |
| Switch output format | `OutputWriter.hpp`, `TracingUtils.hpp` | All pipeline blocks |

The `IDataSource` interface insulates `GeneratorBlock` from the pixel origin. The `ITracingOutput` interface insulates `TracingBlock` from the blob destination. Both follow the Open/Closed Principle: extending behaviour (new source, new sink) requires adding a new class, not modifying an existing one.

---

## 7. Unit Test Strategy

Each stage has a dedicated test executable with a custom lightweight test framework (no external dependencies; GTest optional via `CYNLR_USE_GTEST`).

| Executable | Sections | Tests | Status |
|---|---|---|---|
| `test_generator` | 7 (DataPacket, Config, Random, CSV, SPSC, Factory, GeneratorBlock) | 51 | 50 pass / 1 known spec mismatch |
| `test_filter` | 10 (SlidingWindow, Thresholder, Construction, Identity, Convolution, Boundary, Row transitions, Edge cases, Threaded, Symmetry) | 32 | 32 pass |
| `test_labelling` | 11 (Packet layout, LabelMap, RowLabelBuffer, Construction, Single component, Merge events, Recycling, Row transitions, Coordinates, Memory, Threaded) | 60+ | All pass |
| `test_tracing` | 15 (CompletedBlob, BlobAccumulator, Output sinks, Construction, Single pixel, Horizontal/vertical runs, Merge, Recycle, Background, Multiple blobs, Coordinates, Memory, Flush, Threaded) | 55+ | All pass |

**Key test principles:**
- Every synchronous filter test uses `manualFilter()` as an independent oracle computed from first principles.
- Every tracing test verifies the **pixel conservation invariant**: `sum(blob.pixel_count) == count(foreground pixels in input)`.
- Threaded integration tests exercise the full multi-thread lifecycle (start, run, stop, join, drain) under real concurrent conditions.
- Edge cases: m=2 (minimum), m=1000 (stress), empty input, EOF without trailing blank rows, Windows `\r\n` line endings.

---

## 8. Build Instructions (Quick Reference)

Open **x64 Native Tools Command Prompt for VS** and navigate to the project root:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the threaded pipeline:
```bat
build\bin\CynLr_multiThread.exe --config=src\config.cfg
```

Run unit tests:
```bat
build\bin\test_generator.exe
build\bin\test_filter.exe
build\bin\test_labelling.exe
build\bin\test_tracing.exe
```

Full instructions, CLI flags, and `config.cfg` reference are in `RUN_Instructions.md`.

---

## 9. C++ Standard and Compiler Notes

- **Standard:** C++17 (`std::filesystem`, `if constexpr`, structured bindings, `std::optional` not used but available).
- **Compiler:** MSVC (Visual Studio 2017+) primary target. GCC 15.2 (MinGW64) used for performance profiling.
- **MSVC-specific:** `#pragma warning(disable: 4324)` suppresses the expected padding warning from `alignas(64)` on atomic members. `_mm_pause()` used for spin-wait; `__builtin_ia32_pause()` fallback for GCC/Clang.
- **Windows API:** `SetProcessAffinityMask`, `SetPriorityClass` used in `main.cpp` and `main_linear.cpp` for core pinning and priority elevation. Linked against `winmm` for timer resolution.
- **No external dependencies:** No Boost, no GSL, no third-party libraries. All functionality implemented from standard C++17 and Windows headers.
