# TestTracing.cpp — Test Coverage Summary

## Test Framework
Same lightweight custom framework as TestGenerator / TestFilter / TestLabelling — no external dependencies. Each test registers statically and runs in declaration order. Failures throw `TestFailure`; unexpected exceptions are caught and reported.

## Helper Classes and Functions

`CaptureSink : ITracingOutput`
Collects every emitted `CompletedBlob` into a `std::vector`. Used as the output sink for all synchronous tracing tests. Allows post-run assertion over the complete blob sequence without any file I/O.

`tracingConfig(columns)`
Builds a minimal `SystemConfig` for tracing tests: columns, 1 ms cycle time, threshold=128, default spec kernel, `write_output=false`, `boundary_policy=ZERO_PAD`. All tracing tests use this to avoid config construction boilerplate.

`runChain(columns, rows_data)`
Runs the full Labelling + Tracing chain synchronously. Packs a flat binary pixel grid into `FilteredPacket` pairs, pushes them into a `SimpleQueue`, runs `LabellingBlock` (drain-then-exit), then runs `TracingBlock` (drain-then-exit). Returns all emitted `CompletedBlob` records. This is the primary end-to-end test harness.

`totalPixels(blobs)`
Sums `pixel_count` across all blobs. Used in the **pixel conservation invariant**: `totalPixels(blobs) == totalForegroundPixels(input)`. This invariant is checked in every data test.

`totalForegroundPixels(rows_data)`
Counts non-zero values in the raw input grid. The oracle for the pixel conservation check.

### The Pixel Conservation Invariant
Every test that processes foreground pixels asserts `totalPixels(blobs) == totalForegroundPixels(input)`. This single check simultaneously verifies: no pixels are double-counted across merged blobs, no pixels are silently dropped when labels are recycled, merge accumulator logic correctly transfers pixel counts, and the exit flush emits all remaining blobs. It is the most comprehensive single correctness assertion in the test suite.

---

## Section 1 — CompletedBlob Layout

`blob_sizeof_is_48`
Verifies `sizeof(CompletedBlob) == 48`. Layout: `uint16_t label` (2) + `uint8_t _pad[6]` (6) + five `uint64_t` fields (40) = 48. A size change indicates a layout or alignment change that would break any future queue use or binary serialisation.

`blob_is_trivially_copyable`
`std::is_trivially_copyable<CompletedBlob>` must be true. Required for safe placement in SPSC ring buffers or memory-mapped output files in future pipeline extensions.

`blob_zero_initialised_by_default`
`CompletedBlob{}` must zero all six fields. A non-zero default would produce phantom blob statistics.

---

## Section 2 — BlobAccumulator

`accumulator_inactive_by_default`
A freshly constructed `BlobAccumulator` must have `active == false` and `pixel_count == 0`. A slot that has never been used must not appear in flush output.

`accumulator_update_single_pixel`
`update(5, 10)` on an inactive accumulator: `active` becomes true, `pixel_count == 1`, `top_row == bottom_row == 5`, `left_col == right_col == 10`. Verifies the sentinel values (UINT64_MAX for top_row and left_col) are replaced on first update.

`accumulator_update_expands_bounds`
Three updates at (3,7), (8,2), (5,11). Verifies `top_row=3`, `bottom_row=8`, `left_col=2`, `right_col=11`, `pixel_count=3`. Tests min/max expansion in all four directions.

`accumulator_merge_combines_stats`
Two accumulators A and B with non-overlapping bounding boxes. `A.merge(B)` must produce the union bounding box and summed pixel count. Core correctness of the merge path.

`accumulator_merge_empty_into_active`
Merging an inactive accumulator into an active one must not corrupt the active accumulator. The inactive accumulator has all-sentinel values — the merge must handle the UINT64_MAX sentinels gracefully (min/max comparisons with uninitialised sentinels must not expand the bounding box).

`accumulator_finalise_produces_correct_blob`
Two updates then `finalise(label_id)`. Verifies all six fields of the resulting `CompletedBlob` match the accumulated values. The sentinel-to-zero collapse for `top_row` and `left_col` is verified.

`accumulator_reset_clears_all`
After `reset()`, `active == false` and `pixel_count == 0`. Tests the recycling path — a reset accumulator must be indistinguishable from a freshly constructed one.

`accumulator_finalise_after_reset_returns_zero_blob`
A reset accumulator finalised immediately produces a blob with `pixel_count=0` and `top_row=0` (sentinel collapsed). Tests the edge case where a label is recycled before any pixel is assigned to it — should produce a zero-pixel blob, not crash or produce garbage.

---

## Section 3 — ITracingOutput Implementations

`null_output_does_not_crash`
Calls `emit()` and `flush()` on `NullTracingOutput`. Verifies no exception is thrown. The zero-overhead path must be completely safe.

`capture_sink_records_blobs`
Emits two blobs with distinct labels and pixel counts. Verifies both appear in `sink.blobs` in order with correct fields. Core sanity check for the test harness itself.

---

## Section 4 — TracingBlock Construction Guards

`block_zero_columns_throws`
`TracingBlock` with `columns=0` must throw `std::invalid_argument` before any processing. Zero columns would make `max_labels = 0` — the LabelMap constructor would also throw, but the guard in TracingBlock fires first.

`block_odd_columns_throws`
`columns=5` must throw. Odd columns violate the 2-pixels-per-packet pipeline contract.

`block_valid_columns_constructs`
`columns=10` constructs without throwing. Smoke test.

`block_empty_input_no_blobs`
Zero input packets → zero blobs emitted. Verifies the loop exits cleanly and the exit flush emits nothing (no active accumulators).

`block_all_background_no_blobs`
All-zero grid → zero blobs, and `totalPixels(blobs) == 0 == totalForegroundPixels(input)`. Conservation invariant with zero foreground.

---

## Section 5 — Single Pixel

`single_pixel_one_blob`
Grid: `[1,0,0,0]` then two blank rows. Exactly one blob with `pixel_count=1` must be emitted. The recycle event fires when the blank row is processed, triggering blob emission.

`single_pixel_bounding_box_is_1x1`
Width = `right_col - left_col + 1 == 1`. Height = `bottom_row - top_row + 1 == 1`. A single pixel must have a 1×1 bounding box.

Both single-pixel tests also verify the conservation invariant (`totalPixels == 1`).

---

## Section 6 — Horizontal Run

`horizontal_run_single_blob`
All-foreground row `[1,1,1,1]` + two blank rows → exactly one blob, `pixel_count=4`, `left_col=0`, `right_col=3`, `top_row=bottom_row=0`.

`horizontal_run_correct_width`
`[0,1,1,0]` + blanks → one blob, `pixel_count=2`, `left_col=1`, `right_col=2`. Verifies that background pixels at the edges do not inflate the bounding box.

Both tests verify the conservation invariant.

---

## Section 7 — Vertical Run

`vertical_run_single_blob`
4 rows × 2 cols, col 0 foreground throughout. The blob never receives a recycle event (no blank rows after). `run()` must flush it on exit. Result: one blob, `pixel_count=4`, `top_row=0`, `bottom_row=3`, `left_col=right_col=0`.

`vertical_run_correct_height`
Rows 1 and 2 foreground at col 0; rows 0 and 3 background. One blob with `top_row=1`, `bottom_row=2`. Verifies that the vertical bounding box reflects only actual pixel rows, not the full scan range.

Both tests verify the conservation invariant.

---

## Section 8 — Merge Event → Combined Accumulator

`u_shape_merge_single_blob`
U-shape (3 rows: two arms + closing base) + two blank rows. The merge event fires when the base row is processed. All 7 foreground pixels must appear in exactly one blob: `pixel_count=7`.

`u_shape_bounding_box_correct`
Same U-shape. `top_row=0`, `bottom_row=2`, `left_col=0`, `right_col=2`. The bounding box must encompass both arms and the base.

`merge_combines_pixel_counts`
U-shape: arm A has 2 pixels (rows 0–1 col 0), arm B has 2 pixels (rows 0–1 col 2), base has 3 pixels (row 2 cols 0–2). Total = 7. `totalPixels(blobs) == 7 == totalForegroundPixels(input)`. Verifies that the merge accumulator correctly transfers both arms' pixel counts into the surviving accumulator before the base pixels are added.

`merge_combined_bounding_box_is_union`
Two rectangular blobs (each 2×2) connected by a horizontal base row. One blob with `left_col=0`, `right_col=4`. Verifies the bounding box of the merged blob is the union of both original bounding boxes.

All merge tests verify the conservation invariant.

---

## Section 9 — Recycle Event → Blob Emitted

`recycle_emits_before_reuse`
Blob at row 0 + two blank rows + second blob at row 3. Exactly two blobs must be emitted. The first blob's label is recycled (and potentially reused for the second blob) — both blobs must be distinct in the output.

`two_blobs_correct_pixel_counts`
Same two-blob grid. `blobs[0].pixel_count == 2`, `blobs[1].pixel_count == 2`. Verifies that recycling the first blob does not contaminate the second blob's pixel count accumulator.

`two_blobs_disjoint_coordinates`
`blobs[0].top_row != blobs[1].top_row`. The two blobs' row coordinates must not overlap — they are on distinct rows separated by blank rows.

---

## Section 10 — Background Pixels

`all_background_no_blobs`
12-pixel all-zero grid → zero blobs. Conservation: `totalPixels == 0`.

`background_pixel_in_foreground_row_excluded`
`[1,0,1,0]` + blanks. Two foreground pixels separated by a gap of 1 → two blobs, `totalPixels == 2`. Verifies background pixels at columns 1 and 3 are excluded from blob pixel counts.

---

## Section 11 — Multiple Blobs

`three_isolated_blobs`
`[1,0,1,0,1,0]` + blanks in a 6-column grid. Three blobs, `totalPixels == 3`. The minimum column gap (2 between foreground pixels) is sufficient to prevent 8-connected merging.

`blobs_on_separate_rows`
Blobs at rows 0, 2, and 4 (each separated by a blank row). Three blobs in total. `totalPixels == totalForegroundPixels(input)`. Tests the recycle-then-reallocate sequence across multiple row transitions.

---

## Section 12 — Coordinate Accuracy

`bounding_box_single_row_wide_blob`
Foreground at cols 1–4 of a 6-column row. `left_col=1`, `right_col=4`, `pixel_count=4`. Verifies that the leftmost and rightmost foreground columns set the bounding box precisely, not the row width.

`bounding_box_multi_row_blob`
Staircase blob (row 0: col 0; row 1: cols 0–1; row 2: cols 0–2). `top_row=0`, `bottom_row=2`, `left_col=0`, `right_col=2`, `pixel_count=6`. Tests bounding box expansion as the blob grows across rows and rightward.

`total_pixel_count_matches_foreground_count`
4×4 checkerboard (8-connected: all 8 pixels form one component via diagonal links). `totalPixels(blobs) == 8 == totalForegroundPixels(input)`. The conservation invariant serves as a complete correctness check here — any double-counting or dropped pixel would violate it.

---

## Section 13 — Memory Constraint

`peak_active_never_exceeds_m_over_2`
m=20, 50 rows, worst-case alternating pattern. After the full run: `tracer.peak_active_accumulators() <= 20/2 == 10`. Also verifies the conservation invariant. This is the primary memory-constraint stress test for the Tracing stage.

`total_pixels_conserved_across_blobs`
5 isolated foreground pixels in a 10-column row. `totalPixels(blobs) == 5`. Belt-and-suspenders conservation check for a multi-blob scenario.

---

## Section 14 — Active Blobs Flushed at run() Exit

`active_blobs_flushed_on_exit`
A single all-foreground row `[1,1,1,1]` with no blank rows after it. The LabellingBlock never emits a recycle event (the label is still COOLING when the source exhausts). `TracingBlock::run()` must flush all active accumulators before returning. Result: exactly one blob, `pixel_count=4`. Conservation: `totalPixels == 4`.

This test is critical for CSV mode — without the exit flush, the final blob(s) at the bottom of every CSV input would be silently lost.

---

## Section 15 — Threaded Integration

`threaded_pipeline_does_not_crash`
Smoke test: all four stages (Generator, Filter, Labeller, Tracer) run on dedicated threads concurrently for 50 ms in random mode. The test verifies: no crash, no deadlock, orderly shutdown (stop → join for each thread in pipeline order), and that either some blobs were completed or some packets were processed — confirming the tracing stage received data. This is the only test exercising the full four-stage concurrent pipeline lifecycle.
