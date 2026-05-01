## TestFilter.cpp — Test Coverage Summary

## Test Helpers
`specKernel()`
Returns the exact 9-tap Gaussian kernel from the spec. Used wherever the test needs to replicate real pipeline behaviour.


`identityKernel()`
Centre tap = 1.0, all others = 0. Filtered value equals the raw centre pixel exactly. Used to isolate threshold logic from convolution math — if a threshold test fails with this kernel, the bug is in thresholding not filtering.

`uniformKernel()`
All taps = 1/9. Filtered value equals the arithmetic mean of the 9-element window. Used where a predictable non-trivial filtered value is needed without doing the full spec-kernel arithmetic by hand.

`runFilter()`
Packs a flat uint8 vector into DataPacket pairs with correct row/col tags, pushes them into a SimpleQueue, calls filter.stop() then filter.run() synchronously, and returns all FilteredPacket outputs. This is the core test harness — every synchronous filter test goes through it. Calling stop() before run() means the filter drains the queue then exits without spinning.

`manualFilter()`
Reference implementation of the convolution. For each pixel index, it constructs the 9-element window manually respecting boundary policy, multiplies by the kernel, and sums. Used as the ground-truth oracle in correctness tests.

`writeTempCSV()`
Writes a string to a temp file and returns the path. Used only in the threaded integration tests that need a real CSV on disk.


## Section 1 — SlidingWindow

`window_zero_capacity_throws`
Constructs SlidingWindow(0). Verifies the constructor throws std::invalid_argument. A zero-capacity window would cause modulo-by-zero in the ring indexing.

`window_not_full_until_capacity_reached`
Pushes 4 elements into a capacity-5 window and checks is_full() is false each time. Pushes the 5th and checks is_full() is true. Verifies the fill counter increments correctly and the full condition fires at exactly the right point.

`window_centre_is_correct_index`
Fills a capacity-9 window with values 0, 10, 20 ... 80 and checks centre().value == 40 and centre().col == 4. Confirms centre() returns at(capacity/2) — the pixel being convolved — not the head or tail.

`window_ring_buffer_wraps_correctly`
Fills a capacity-5 window then pushes three more values one at a time. After each push, checks the centre value matches the expected sliding window state. Catches any head-pointer or modulo bug that causes the ring to corrupt its logical ordering after a wrap.

`window_reset_clears_state`
Fills a window to capacity, calls reset(), then verifies is_full() is false and filled() is 0. Confirms the row-transition reset actually clears the buffer — if it doesn't, the next row's left-pad would be applied on top of stale data.

`window_at_returns_logical_order`
Pushes values 10–14 into a capacity-5 window, then checks at(0) through at(4) return 10, 11, 12, 13, 14 in insertion order. Confirms the logical-index-to-physical-slot mapping is correct before any wrap has occurred.

`window_at_after_multiple_wraps`
Pushes 20 values into a capacity-5 window (4 full wrap-arounds). Checks at(0)==15, at(4)==19, centre()==17. The most thorough ring-buffer test — catches any accumulated index drift after repeated head pointer advancement.


## Section 2 — BinaryThresholder

`thresholder_at_boundary`
Applies exactly TV=128 and TV-0.1=127.9. Verifies the boundary is inclusive on the upper side (>= TV → 1) and exclusive on the lower side (< TV → 0). Catches an off-by-one in the comparison operator.

`thresholder_zero_threshold_all_ones`
Sets TV=0 and applies 0.0 and 255.0. Both must return 1. Confirms that even a zero filtered value passes a zero threshold — >= 0 is always true for non-negative inputs.

`thresholder_max_threshold`
Sets TV=255 and applies 254.9, 255.0, 256.0. Verifies only values at or above 255 return 1. Catches any float precision issue at the upper boundary of uint8 range.

`thresholder_set_threshold_updates_behaviour`
Constructs with TV=100, verifies behaviour, then calls set_threshold(50) and verifies behaviour changes correctly. Confirms the threshold is stored as a mutable value not a compile-time constant — needed for future dynamic TV updates.

`thresholder_negative_filtered_value`
Applies -1.0 to a TV=0 thresholder. Verifies it returns 0. Negative filtered values are possible with zero-pad boundary when the kernel has positive taps and edge pixels are padded with 0 — the thresholder must handle them without undefined behaviour.


## Section 3 — FilterBlock Construction
`filter_empty_kernel_throws`
Constructs FilterBlock with an empty kernel vector. Verifies std::invalid_argument is thrown before the block is used. An empty kernel would cause a zero-capacity SlidingWindow and divide-by-zero in half_width_.

`filter_even_kernel_size_throws`
Constructs with a 4-element kernel. Verifies std::invalid_argument is thrown. An even kernel has no centre element — the convolution is mathematically undefined and capacity/2 would pick the wrong pixel.

`filter_zero_threshold_all_ones`
Feeds four zero-value pixels with TV=0 and the identity kernel. Verifies both output pixels are 1. Confirms that 0.0f >= 0.0f evaluates to true in the threshold comparison — catches any accidental use of strict > instead of >=.


## Section 4 — Identity Kernel Tests

`filter_identity_kernel_all_above_threshold`
Eight pixels all at 200, threshold 100, identity kernel. All 4 output packets must have b1=1, b2=1. Verifies the basic pipeline path — generate packet, push through filter, emit output — works end-to-end with a trivially predictable result.

`filter_identity_kernel_all_below_threshold`
Four pixels all at 50, threshold 100, identity kernel. All 2 output packets must be all zeros. Mirror of the above — confirms the zero branch of the threshold comparison.

`filter_identity_kernel_mixed_values`
Row [50, 200, 50, 200] with threshold 100 and identity kernel. Expected output is 0, 1, 0, 1. Verifies per-pixel independence — that the filter doesn't smear one pixel's output into its neighbour's result.


## Section 5 — Convolution Correctness

`filter_uniform_kernel_single_row_zero_pad`
Row of all zeros except index 4 = 90. Uniform kernel, TV=5. The centre pixel's filtered value is 90/9 = 10.0 ≥ 5, so b1 of packet 2 must be 1. All others see a mean near zero and must be 0. Verifies that the convolution sum is mathematically correct for a controlled non-trivial case.

`filter_spec_kernel_constant_input`
Ten pixels all at 100 with the spec kernel and REPLICATE boundary. Since the spec kernel taps sum to ≈1.0, filtered value ≈ 100 everywhere. Threshold 99 means all outputs must be 1. Confirms the spec kernel coefficients are loaded and applied correctly and that REPLICATE padding doesn't distort a constant signal.

`filter_manual_computation_against_reference`
Row [10, 30, 50, 70, 90, 110, 130, 150, 170, 190] with the spec kernel, TV=80, ZERO_PAD. Computes expected binary output for each pixel using manualFilter() as the oracle, then compares pixel-by-pixel against the actual filter output. This is the most rigorous correctness test — any error in the dot product, window ordering, or padding will show up as a specific pixel mismatch with its index reported.


## Section 6 — Boundary Policy
`filter_zero_pad_reduces_edge_values`
Row of all 200s, spec kernel, TV=150. With ZERO_PAD the first pixel's window contains 4 zeros on the left, giving a filtered value ≈ 134 < 150 → output 0. With REPLICATE the same pixel sees 200 on both sides → output 1. Runs both policies and verifies the first pixel differs. Directly tests that boundary policy affects edge output as mathematically expected.

`filter_replicate_preserves_constant_row`
Constant row of 128s, spec kernel, TV=127, REPLICATE. Every output must be 1. Since REPLICATE extends the constant value into the padding region, every window — including edge windows — contains all 128s, so filtered value ≈ 128 ≥ 127 everywhere. Confirms REPLICATE doesn't introduce any artificial edge rolloff.


## Section 7 — Row Transitions
`filter_multi_row_resets_window`
Two rows: row 0 all zeros, row 1 all 255s, TV=1, REPLICATE. Row 0 outputs must all be 0, row 1 outputs must all be 1, and each packet's row field must match. If the window is not reset between rows, row 1's left-pad would pull in zeros from row 0, causing the first few pixels of row 1 to output 0 incorrectly. This is the most important correctness test 
for multi-row operation.

`filter_row_col_coordinates_correct`
Two rows of 6 columns, identity kernel, constant values. Verifies the row and col fields on every output packet are exactly correct — (0,0), (0,2), (0,4), (1,0), (1,2), (1,4). Confirms that the coordinate tracking in PendingOutput propagates the centre pixel's position correctly through the pairing logic.



## Section 8 — Edge Cases
`filter_minimum_columns_m_equals_2`
Single row of 2 pixels, identity kernel, TV=50. Verifies at least 1 packet is produced with correct b1=1, b2=1. m=2 is the minimum valid column count per the spec — confirms the pipeline doesn't underflow or skip the only packet.

`filter_single_row_exact_output_count`
m=8, 1 row, identity kernel. Verifies exactly 4 packets are produced — one per pair of columns. Catches any off-by-one that produces 3 or 5 packets from 8 pixels.

`filter_empty_input_produces_no_output` 
Empty raw vector. Verifies the output queue remains empty. Confirms the filter handles a zero-packet run without crashing or emitting garbage from uninitialised state.

`filter_large_row_1000_columns`
1000 pixels all at 128, spec kernel, TV=127, REPLICATE. Verifies all 500 output packets have b1=1, b2=1. Stress test for the ring buffer and loop — catches any index overflow, capacity misallocation, or performance-related correctness bug at scale.


## Section 9 — Threaded Integration
`filter_threaded_csv_end_to_end`
Runs GeneratorBlock on a thread with a known CSV row, joins it, then runs FilterBlock synchronously to drain. Compares every output bit against manualFilter(). The only test that exercises the full Generator → queue → Filter pipeline with real inter-thread data transfer. Catches any packet ordering, row/col tagging, or queue handoff bug that pure unit tests cannot see.

`filter_threaded_random_does_not_crash`
Smoke test: runs Generator and Filter concurrently for 50 ms in random mode then shuts down cleanly. Verifies no crash, no deadlock, and at least some output was produced. Does not check values — the point is to confirm the two-thread lifecycle (start, run, stop, join, drain) is correct under real concurrent conditions.


## Section 10 — Kernel Symmetry
`filter_symmetric_input_symmetric_output`   
Symmetric row [10, 50, 100, 200, 255, 255, 200, 100, 50, 10] with the spec kernel and REPLICATE. Verifies bits[i] == bits[COLS-1-i] for all i < COLS/2. Since the spec kernel is symmetric and the input is symmetric, the output must be mirror-symmetric. Any asymmetry reveals a bug in the window's logical ordering — specifically that at(0) and at(8) are not equidistant from centre in the ring buffer's physical layout.
