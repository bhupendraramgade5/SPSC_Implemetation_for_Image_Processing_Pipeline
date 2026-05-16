# TestLabelling.cpp — Test Coverage Summary

## Test Framework
Lightweight custom framework identical to TestGenerator/TestFilter — no external dependencies. GTest-compatible via `CYNLR_USE_GTEST` CMake flag. Each test registers via a static `TestRegistrar` and runs in declaration order. Failures throw `TestFailure`; unexpected exceptions are caught and reported. Final pass/fail count is printed.

## Helper Functions

`labelConfig(columns)`
Builds a minimal `SystemConfig` suitable for labelling tests. Sets columns, a 1 ms cycle time, threshold=128, and the default 9-tap spec kernel. All labelling tests use this rather than constructing configs by hand, ensuring consistency.

`runLabeller(columns, rows_data)`
Packs a flat `uint8` binary grid into `FilteredPacket` pairs (2 pixels per packet, row-major), pushes them into a `SimpleQueue<FilteredPacket>`, calls `block.stop()` then `block.run()` synchronously (drain-then-exit semantics), and returns all `LabelledPacket` outputs as a vector. This is the primary synchronous test harness.

`flattenLabels(packets)`
Extracts `l1` and `l2` from every output packet into a flat per-pixel label vector in raster order. Simplifies index-based assertions.

`countDistinctLabels(labels)`
Returns the count of unique non-zero label values in a label vector. Used to assert the number of distinct components without assuming specific label IDs.

`allSameLabel(labels, mask)`
Returns true if every foreground position in `mask` carries the same non-zero label. Used to verify single-component connectivity without hardcoding label IDs.

---

## Section 1 — LabelledPacket Layout and Static Guarantees

`packet_sizeof_is_32`
Verifies `sizeof(LabelledPacket) == 32`. The layout is fixed: two `uint64_t` (row, col) + seven `uint16_t` (l1, l2, merge_old, merge_new, merge_old2, merge_new2, recycled) + two pad bytes = 32. A size change indicates a field was added or alignment changed — both break the SPSC ring buffer's raw-copy assumption.

`packet_is_trivially_copyable`
Verifies the `static_assert` in `LabellingUtils.hpp` also holds at runtime. Required for safe use in SPSC ring buffers that copy via raw assignment.

`packet_zero_initialised_by_default`
Confirms `LabelledPacket{}` zero-initialises all nine fields. Catch: a non-zero default would silently produce spurious merge or recycle events in tests that check field values.

`packet_fields_are_independent`
Writes distinct values to row, col, l1, l2, merge_old, merge_new. Verifies reading them back returns the same values and that merge_old2 (untouched) is still zero. Guards against struct packing or bitfield bugs.

`packet_copy_is_independent`
Copies a packet, mutates the copy's l1, and verifies the original is unchanged. Rules out any accidental pointer or reference member.

`packet_alignof_is_8`
Verifies `alignof(LabelledPacket) == 8` — the alignment required for the `uint64_t` members. Misalignment would cause undefined behaviour on strict-alignment platforms.

---

## Section 2 — LabelMap

`labelmap_zero_max_throws`
Constructs `LabelMap(0)`. Verifies `std::invalid_argument` is thrown. A zero-capacity map would cause index-out-of-range on the very first `newLabel()` call.

`labelmap_new_label_starts_at_1`
First `newLabel()` must return 1, not 0. Label 0 is permanently reserved as the background sentinel — returning it from `newLabel()` would corrupt the labelling logic.

`labelmap_new_label_increments`
Three consecutive calls return 1, 2, 3 in order. Confirms `next_fresh_` increments monotonically.

`labelmap_find_before_merge_returns_self`
Before any `unite()`, `find(a)` must return `a`. Verifies that construction correctly initialises `parent_[i] = i` for all slots.

`labelmap_find_background_returns_0`
`find(0)` must return 0 without accessing `parent_[0]` (which is not a valid label). Guards against an off-by-one that reads `parent_[0]`.

`labelmap_unite_lower_label_survives`
`unite(2, 1)` must return 1 and `find(2)` must return 1. The lower label always survives — this matches LabellingBlock's merge event semantics where `merge_new < merge_old`.

`labelmap_unite_same_component_noop`
`unite(a, a)` must return `a` and leave `active()` unchanged. Guards against a double-decrement of the active counter.

`labelmap_unite_chain_of_three`
Sequential `unite(2,1)` then `unite(3,1)`. Verifies `find(3) == 1` after two merges — path-halving must eventually resolve all aliases to the root.

`labelmap_active_count_tracks_correctly`
Active count starts at 0, increments on `newLabel()`, decrements on `unite()`. Tests the full lifecycle without recycle.

`labelmap_full_returns_true_at_capacity`
After allocating all `max_labels` slots, `full()` must be true. Confirms the capacity boundary.

`labelmap_new_label_returns_0_when_full`
An allocation attempt on a full map must return 0 (background sentinel), not crash. The caller (LabellingBlock) treats label 0 as a hard invariant violation — this test ensures the failure mode is detectable, not silent corruption.

`labelmap_recycle_frees_slot`
After recycling one label from a full map, `full()` is false and `newLabel()` succeeds. Verifies the free list round-trip.

`labelmap_recycle_ignores_background`
`recycle(0)` must not decrement `active_` or corrupt the free list. Guards against a guard-clause omission.

`labelmap_path_halving_flattens_chain`
Manually builds a 4-level parent chain (4→3→2→1 via repeated unites), then calls `find(4)`. Verifies it resolves to 1 and that path-halving writes back intermediate nodes so subsequent `find(3)` also returns 1 in O(1).

`labelmap_reset_clears_all_state`
After reset, `active() == 0`, `full() == false`, and `newLabel()` returns 1. Confirms the map is back to construction state — needed for test isolation in suites that share a LabelMap.

---

## Section 3 — RowLabelBuffer

`rowbuf_zero_columns_throws` / `rowbuf_zero_max_labels_throws`
Construction guard tests. Either a zero-column or zero-max-label buffer would cause divide-by-zero or index-out-of-range in the hot path.

`rowbuf_curr_initially_all_zero` / `rowbuf_prev_initially_all_zero`
Both label arrays must be zero at construction — no stale label IDs from uninitialized memory.

`rowbuf_set_and_curr_roundtrip`
`set(c, l)` then `curr(c)` returns `l`. `curr()` at an unset column returns 0. Core correctness of the write path.

`rowbuf_oob_curr_returns_zero` / `rowbuf_oob_prev_returns_zero`
Out-of-range column access (including `col == columns` which is the NE boundary case for the last column) returns 0 without crashing. Critical for `neighbourNE()` which reads `prev(col+1)`.

`rowbuf_commit_moves_curr_to_prev`
After `commitAndRecycle()`, values written to `curr_` during the row are readable from `prev_()` for the next row. Verifies the buffer swap.

`rowbuf_commit_zeros_curr`
After the commit, `curr_` is cleared. Verifies that the new row starts with a clean slate — no contamination from the previous row.

`rowbuf_first_commit_no_recycles`
The very first commit (prev was all zeros) must return 0 recycling candidates. Confirms the presence-flag comparison correctly identifies that no labels existed in the initial prev.

`rowbuf_second_commit_recycles_missing_label`
Row 0: labels 1,2,3. Row 1: only labels 1,2. `commitAndRecycle()` must report exactly one recycled label (3). The most direct test of the recycling decision logic.

`rowbuf_drain_dead_from_prev_basic`
After a commit (labels 1,2,3 in prev), with curr empty, `drainDeadFromPrev(3)` must report 2 dead labels (those at prev columns 0 and 2 whose NE reach is now behind completed_col=3).

`rowbuf_drain_skips_label_present_in_curr`
If label 1 has been inherited into curr, `drainDeadFromPrev()` must not report it as dead even though it appears in prev. The COOLING→LIVE transition must suppress the drain for that slot.

`rowbuf_commit_after_drain_no_double_report`
Labels drained mid-row must not be re-reported by `commitAndRecycle()` at the row boundary. The presence flag cleared by `drainDeadFromPrev()` must prevent the label from appearing in the commit's recycle list.

`rowbuf_reset_clears_all`
After `reset()`, both prev and curr are zero. Tests the hard reset path used in construction and between test runs.

---

## Section 4 — LabellingBlock Construction Guards

`block_zero_columns_throws`
`LabellingBlock` with `columns=0` must throw `std::invalid_argument` before any processing occurs.

`block_odd_columns_throws`
`columns=5` (odd) must throw. The pipeline contract requires even columns because packets always carry exactly 2 pixels.

`block_valid_columns_constructs`
`columns=10` constructs without throwing. The simplest smoke test for the happy path.

`block_empty_input_produces_no_output`
Zero packets in → zero packets out. Verifies the loop exits cleanly on the very first pop when stop() has already been called.

`block_all_zeros_produces_zero_labels`
A grid of all zeros must produce all-zero labels. Background pixels must never receive a non-zero label.

---

## Section 5 — Single Connected Component (No Merges)

`single_pixel_gets_nonzero_label`
`[1, 0]` in a 1×2 grid: l1 must be non-zero, l2 must be zero. The simplest possible foreground pixel.

`two_adjacent_horizontal_pixels_same_label`
`[1, 1]`: l1 == l2 and both non-zero. The W neighbour of the second pixel must resolve to the first pixel's label.

`horizontal_run_all_same_label`
6 pixels all foreground: exactly 1 distinct label across all outputs. Tests the W-neighbour chain across a full row.

`vertical_run_same_label`
3 rows × 2 cols, col 0 foreground: pixels at rows 0,1,2 col 0 must share one label. The N-neighbour links rows together.

`diagonal_NW_connected`
`(0,0)=1`, `(1,1)=1`, all others 0. The NW neighbour lookup must connect them. Verifies the `neighbourNW()` helper.

`diagonal_NE_connected`
`(0,1)=1`, `(1,0)=1`. The NE neighbour at (1,0) is prev[1]=(0,1) — must connect. Verifies `neighbourNE()`.

`three_by_three_blob_single_label`
3×4 grid with a solid 3×3 foreground block. All 9 foreground pixels must carry the same label — exhaustive connectivity test for the 4-causal neighbourhood.

`two_isolated_blobs_different_labels`
`[1, 0, 1, 0]`: two foreground pixels separated by a gap of 1. Both non-zero, both distinct. Verifies label isolation when no causal neighbour connects two pixels.

---

## Section 6 — Merge Events

`u_shape_merge_fires_at_closing_pixel`
The canonical merge correctness test. Two arms of a U start with different labels; the base row closes the U. Asserts: (a) at least one merge event appears in the base row's packets, (b) all foreground pixels in the base row carry the surviving (lower) label, (c) the surviving label equals `min(arm_left, arm_right)`.

Note: rows 0–2 packets are NOT retroactively updated — streaming semantics means old packets keep their original labels. The merge event is how the Tracing block learns they became one component.

`u_shape_merge_surviving_label_is_lower`
For every merge event in the output, `merge_new < merge_old`. The lower label always survives — consistent with `LabelMap::unite()` which always roots the higher label under the lower one.

`l_shape_single_label_no_merge`
An L-shape (vertical + horizontal arm sharing a corner pixel) is one component reachable through a single causal chain. No merge events should fire — the connectivity is established purely through W and N neighbours without ever encountering two distinct roots simultaneously.

`cross_shape_four_arms_single_label`
A plus/cross shape in a 5×6 grid. The centre row connects the vertical arm (top) with the horizontal arm. Asserts: (a) a merge fires in the centre row, (b) the arm label (assigned in rows 0–1) is the surviving label, (c) all post-merge pixels in rows 3–4 carry the surviving label, (d) centre-row packets after the merge packet all carry the surviving label.

`merge_old_label_not_assigned_after_merge`
After the first packet containing a merge event, neither `l1` nor `l2` of any subsequent packet may carry the absorbed (`merge_old`) label. This verifies that `find()` correctly resolves all subsequent pixels through the merged path.

---

## Section 7 — Label Recycling

`completed_blob_recycle_event_fires`
A foreground blob in row 0, followed by two blank rows. A `recycled != 0` event must appear somewhere in the blank-row packets. Confirms the row-boundary recycle path fires.

`recycled_label_id_was_previously_assigned`
The label ID in a `recycled` field must have appeared as l1 or l2 in an earlier packet. Verifies that recycled IDs are always labels that were actually allocated and assigned — not fabricated values.

`peak_active_labels_never_exceeds_m_over_2`
Worst-case pattern: alternating 1-0 per row, 50 rows, m=20. Checks that no foreground pixel receives label 0 (which would indicate LabelMap exhaustion). Demonstrates the mid-row drain keeps the map perpetually below capacity.

`infinite_stream_label_map_never_exhausted`
200 rows of the same alternating pattern, m=30. For every output packet where `fp.b1 == 1`, `lp.l1 != 0`. For every `fp.b2 == 0`, `lp.l2 == 0`. If the map ever exhausted, foreground pixels would silently receive label 0 — a correctness failure invisible without this test.

---

## Section 8 — Row Transitions

`row_transition_resets_window`
Two blobs in different rows (rows 0 and 2) separated by a blank row. Asserts: (a) both blobs are foreground, (b) a recycle event fires in the blank row — confirming `commitAndRecycle()` ran and the RowLabelBuffer was correctly swapped, (c) blob B carries no merge events (it is a fresh component, not connected to blob A).

`cross_row_connectivity_preserved`
A vertical stripe across 4 rows × 2 cols. All foreground pixels in col 0 must carry the same label across all rows. Tests N-neighbour inheritance through multiple row transitions.

`separate_blobs_on_adjacent_rows_different_labels`
Row 0: foreground at col 0 only. Row 1: foreground at col 2 only. Column gap = 2, so no NW/N/NE bridge exists between them. Asserts no merge events and that a recycle fires (blob A completes when row 1 is processed). This test carefully documents the 8-connectivity geometry.

`row_packet_count_matches_columns`
3 rows × 8 cols: must produce exactly 3 × (8/2) = 12 output packets. Verifies no packet is dropped or duplicated across row boundaries.

---

## Section 9 — Coordinate Correctness

`packet_row_matches_input_row`
3 rows × 4 cols, all foreground. Verifies every output packet's `row` field matches the scan row of its input — row 0 packets have row=0, row 1 have row=1, etc.

`packet_col_matches_l1_position`
m=6: packets must have col = 0, 2, 4. Confirms the 2-pixel stride in the column coordinate.

`l1_is_col_l2_is_col_plus_one`
`[1, 0, 1, 0]` in one row: packet 0 has l1=foreground (col 0), l2=background (col 1). Packet 1 has l1=foreground (col 2), l2=background (col 3). Verifies the b1/b2 → l1/l2 pixel assignment mapping.

`coordinates_two_rows_exact`
2 rows × 4 cols, all background. Verifies all four packets have exact (row, col) pairs: (0,0), (0,2), (1,0), (1,2). Tests coordinate tracking end-to-end even with no foreground pixels.

---

## Section 10 — Memory Constraint

`minimum_columns_m_equals_2`
m=2 is the smallest valid even column count. One row `[1,1]` must produce exactly 1 packet with l1 == l2 (same label). Verifies the minimum boundary of the pipeline contract.

`large_m_produces_correct_packet_count`
m=130, 5 rows: must produce exactly 5 × 65 = 325 packets. Confirms no packets are dropped or duplicated at larger column counts.

`large_row_no_label_exhaustion`
m=130, 20 rows, alternating pattern. Mid-row drain must keep LabelMap below its m/2=65 capacity. Any foreground pixel receiving label 0 is counted as a failure. This is the primary memory-constraint stress test at realistic m values.

---

## Section 11 — Threaded Integration

`threaded_csv_end_to_end_known_grid`
Directly pushes a known U-shape grid (3 rows × 4 cols) into the labeller via `FilteredPacket` from the main thread, runs `LabellingBlock` on a dedicated thread with a 5 ms window, then verifies: (a) 6 output packets (3 rows × 2 packets/row), (b) a merge fires in the base row, (c) all foreground pixels after the merge carry the surviving (lower) arm label. This is the only test that exercises the full inter-thread lifecycle.

`threaded_random_does_not_crash`
Smoke test: a producer thread pushes random `FilteredPacket` values for 50 ms while `LabellingBlock::run()` consumes them concurrently. Verifies no crash, no deadlock, and that at least some output was produced. Tests the two-thread lifecycle (start, run, stop, join) under real concurrent conditions with DynamicSPSCQueue back-pressure.
