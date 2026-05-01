#  Linear Pipeline — Design Document
## 1. Purpose

CynLr_Linear is a single-threaded, queue-free variant of the main pipeline. It exists purely as a measurement baseline — to isolate the raw algorithmic cost of the 9-tap convolution + threshold from all threading, queue, and synchronisation overhead.

## 2. High-Level Architecture

```
source->next(packet)
    │
    ▼
LinearFilter::beginRow()       ← called once per row transition
LinearFilter::processSample()  ← called twice per packet (v1, v2)
LinearFilter::flush()          ← called at end of each row
    │
    ▼
IOutputWriter::write()
    │
    ▼
timestamp recorded → pixel_timestamps[]
```

## 3. Key Classes and Responsibilities

The core of the linear pipeline. Owns a SlidingWindow ring buffer of size 9 (kernel size). Three entry points:

* `beginRow(left_edge, row)` — resets the window, left-pads with half_width replicated or zero-padded values per BoundaryPolicy
* `processSample(value, row, col, fp)` — pushes one pixel into the window; once full, runs dotProduct(), thresholds, stages into PendingOutput; returns true and fills fp when a complete pair (b1, b2) is ready
* `flush(edge, row, last_col, out)` — right-pads the row end, drains any staged unpaired pixel

## 4. Hot Path — dotProduct()
The 9-tap case is fully unrolled at compile time:
```C++
cppreturn static_cast<float>(window_.at(0).value) * k[0]
     + static_cast<float>(window_.at(1).value) * k[1]
     + ...
     + static_cast<float>(window_.at(8).value) * k[8];
```

## 5. Timing Strategy
One `steady_clock::now()` call is made after each FilteredPacket is written. The timestamp is pushed twice into pixel_timestamps — once for b1, once for b2 — so the gap vector has one entry per pixel rather than per packet. Inter-pixel gaps are computed in a single pass after the loop:
```C++
for (size_t i = 1; i < pixel_timestamps.size(); ++i)
    if (timestamps[i] >= timestamps[i-1])
        gaps.push_back(timestamps[i] - timestamps[i-1]);
```
`computeLinearStats()` then sorts the gap vector once and returns min / avg / p50 / p99 / max.

## 6. Termination

Three independent stop conditions, checked at the top of each loop iteration:

`g_stop` — set by SIGINT / SIGTERM signal handler
`deadline` — run_duration_ms elapsed
`max_rows` — row count reached
`source->next()` returning false — CSV exhausted

## 7. Process Affinity and Priority
```Shell
SetProcessAffinityMask(GetCurrentProcess(), 1);  // pin to Core 0
SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
```
Core pinning eliminates cache migration cost between measurements. High priority reduces OS preemption during the run. This mirrors the threaded pipeline's settings so results are comparable.

## 8. Drawbacks
1. **Not the production path**

    * The linear pipeline has no concept of back-pressure, pipeline depth, or concurrent processing. It cannot be extended to feed the Labelling block (next evaluation stage) without a complete rewrite into the threaded model.
2. **Timestamp granularity distorts short runs**

    * steady_clock on Windows has ~100 ns resolution in practice (backed by QueryPerformanceCounter). For rows where the filter finishes in under 100 ns, consecutive timestamps are identical — those pairs produce a gap of 0 and are silently dropped from the stats. This means the reported minimum is artificially floored.
3. **Double-timestamp per packet inflates sample count**

    * Pushing the same timestamp twice per packet makes the gap vector treat b1→b2 as a 0 ns gap. This pulls the average and p50 down and inflates count by roughly 2×. It is a known approximation, not a real measurement.
4. **OutputWriter sits inside the timed loop**

    * IOutputWriter::write() is called between the filter step and the timestamp. If write_output = true, file I/O latency is folded into every gap measurement. Disable write_output for clean perf numbers.
5. **Single-core only**

    * Pinned to Core 0. On a machine with other high-priority processes on Core 0, results will show high p99 variance. The threaded pipeline can spread across two cores and absorb this better.
6. **No pipeline overlap**

    * Generate and filter run sequentially in the same call stack. In the threaded pipeline, the generator is producing the next packet while the filter is processing the current one. The linear model always pays both costs serially, so its throughput ceiling is lower than the threaded model under real scan rates.
7. **Memory grows unbounded**

    * pixel_timestamps pre-allocates 2 million entries but will grow beyond that with push_back if the run is long enough. For a 5-second run at T=1000 ns this is fine; for unlimited runs it is a silent memory leak.

## 9. Performance Results