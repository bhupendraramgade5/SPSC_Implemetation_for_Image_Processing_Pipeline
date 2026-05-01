# TestGenerator.cpp — Test Coverage Summary

## Section 1 — DataPacket
`datapacket_zero_initialised_by_default` : Confirms that DataPacket{} zero-initialises all four fields. Catches any accidental non-zero defaults that could cause silent bugs in the pipeline's row/col tracking.

`datapacket_fields_are_independent` : Writes distinct values to all four fields and reads them back. Catches struct packing or bitfield bugs where writing one field corrupts an adjacent one.

`datapacket_is_trivially_copyable` : Verifies the static_assert that SPSCQueue relies on at compile time also holds at runtime via std::is_trivially_copyable. If this fails, the ring buffer's raw memory copy semantics are unsafe.

`datapacket_copy_is_independent` : Copies a packet and mutates the copy. Confirms the copy is a value not a reference — rules out any accidental pointer or reference member inside the struct.

## Section 2 — ConfigManager

`config_default_kernel_applied_when_missing`
Writes a config file with no kernel line. Verifies that ConfigManager fills in the 9-tap default kernel and parses m, T, threshold correctly.

`config_csv_mode_parsed_correctly`
Writes a config with mode=csv and input_file. Verifies both fields parse correctly and Mode::CSV is set.

`config_zero_columns_throws`
Feeds m=0. Verifies validate() catches it and throws std::runtime_error before any block is constructed.

`config_zero_cycle_time_throws`
Feeds T=0. Verifies validate() rejects it — a zero cycle time would cause divide-by-zero and infinite spin loops downstream.

`config_custom_kernel_parsed`
Writes a 9-element custom kernel. Verifies all elements parse and the centre value (index 4) is correct. Catches off-by-one errors in the comma-separated parser.

`config_missing_file_uses_defaults_or_warns`
Runs ConfigManager::load from a directory with no config.cfg. Verifies it throws rather than silently using zero-columns defaults that would pass through to the pipeline and crash later.


## Section 3 — RandomDataSource
`random_values_in_uint8_range`
Calls next() 2000 times and checks every v1/v2 is in [0, 255]. Catches any RNG distribution or cast bug that produces out-of-range values.

`random_never_returns_false`
Calls next() 10,000 times and asserts it always returns true. Confirms random mode is infinite — the pipeline must never stop due to source exhaustion in random mode.

`random_col_advances_by_two_within_row`
Takes two consecutive packets from the same row and checks col advances by exactly 2. Validates the generator's column-stepping contract that the filter depends on.

`random_row_wraps_correctly`
Takes three packets from a 4-column source and verifies the sequence is row=0/col=0, row=0/col=2, row=1/col=0. Catches off-by-one in the row wrap logic.

`random_row_wraps_at_column_boundary`
Extends the above to five packets across two full rows. More exhaustive check of the wrap boundary.

`random_coordinate_sequence_exhaustive`
Runs 10 complete rows of 8 columns and verifies every (row, col) pair is exactly correct in order. The most thorough coordinate correctness test.

`random_zero_columns_throws`    
Constructs RandomDataSource(0). Verifies the constructor rejects it with std::invalid_argument rather than dividing by zero in advance().

`random_v1_and_v2_are_independent`
Over 500 samples, checks that v1 and v2 are not always equal. Catches a bug where both pixels are generated from the same RNG call.

`random_produces_varied_values`
Over 500 samples, checks that v1 is not always the same value. Catches a frozen or constant RNG.


## Section 4 — CSVDataSource

`csv_basic_two_rows_four_columns`
Parses a 2-row × 4-column CSV and verifies all four packets have correct v1, v2, row, col. The foundational correctness test for the CSV parser.

`csv_returns_false_at_eof_and_stays_false`
Exhausts a 1-row CSV and calls next() three more times. Verifies EOF returns false consistently — the pipeline loop depends on this to terminate.

`csv_whitespace_around_commas`
Feeds a CSV with spaces around values. Verifies the parser trims whitespace correctly — real CSV files from spreadsheet exports commonly have this format.

`csv_boundary_values_0_and_255`
Feeds the min and max uint8 values. Catches any truncation or sign-extension bug in the stoi → uint8_t cast path.
`csv_row_index_increments_once_per_line`
Reads a 3-row CSV and asserts packet.row increments exactly once per line, not once per packet. Catches a common bug where row_++ is placed inside next() instead of loadNextRow().

`csv_no_trailing_newline`
Feeds a CSV without a final \n. Verifies getline still parses the last row. Many real files omit the trailing newline.

`csv_crlf_line_endings`
Feeds Windows \r\n line endings. Verifies the \r is stripped and values parse correctly — without trimming, stoi("40\r") throws on some platforms.

`csv_empty_file_returns_false_immediately`
Feeds an empty file. Verifies next() returns false on the first call without crashing.

`csv_single_row_two_columns_minimum`
Tests the smallest valid input — one row, two columns, one packet. Verifies the minimum boundary of the pipeline's column contract.

`csv_bad_file_path_throws`
Constructs CSVDataSource with a nonexistent path. Verifies the constructor throws std::runtime_error immediately rather than silently returning false later.

`csv_zero_columns_throws`
Constructs with columns=0. Verifies std::invalid_argument is thrown — zero columns would cause divide-by-zero in advanceCol().

`csv_single_row_two_columns`
Duplicate of the minimum boundary test with an explicit !src.next() assertion. Belt-and-suspenders check for EOF after exactly one packet.

`csv_large_grid_exhaustive_verification`
Generates a 5×6 grid with known values (r*10+c) % 256, writes it to CSV, reads it back, and verifies every single v1/v2/row/col. The most complete end-to-end correctness test for the CSV parser.

## Section 5 — SPSCQueue

`spsc_empty_on_construction`
Verifies empty() and size() are correct on a freshly constructed queue.

`spsc_single_push_pop_roundtrip`
Pushes one packet, pops it, verifies all four fields survived the ring buffer round-trip intact.


`spsc_fifo_ordering_preserved`
Pushes 10 packets, pops all 10, verifies they come out in the same order. Confirms the ring buffer is truly FIFO.

`spsc_pop_on_empty_returns_false`
Pops from an empty queue. Verifies false is returned and the output parameter is not corrupted.

`spsc_push_on_full_returns_false`
Fills a capacity-4 queue to its limit (3 items) then attempts a 4th push. Verifies false is returned — back-pressure signalling.

`spsc_pop_after_full_makes_space`
Fills to capacity, pops one, then pushes again. Verifies a single pop correctly frees exactly one slot.

`spsc_wrap_around_over_many_cycles`
Pushes and pops 16 times through a capacity-4 queue. Forces the head and tail indices to wrap around multiple times, catching any modulo or mask bug.

`spsc_size_tracks_occupancy`
Verifies size() increments on push and decrements on pop. Catches any counter drift.

`spsc_two_thread_1000_packets_ordered`
Runs a real producer thread pushing 1000 packets concurrently with a consumer thread verifying ordering. The only multi-threaded test — catches actual memory ordering bugs that single-threaded tests cannot.


## Section 6 — createDataSource Factory

`factory_random_mode_returns_random_source`
Calls the factory with Mode::RANDOM and verifies the returned source produces packets indefinitely.

`factory_csv_mode_returns_csv_source`
Calls the factory with Mode::CSV and a real file. Verifies exactly 2 packets are produced then EOF.

`factory_csv_bad_path_throws`
Calls the factory with a nonexistent CSV path. Verifies the factory propagates the std::runtime_error from CSVDataSource.

## Section 7 — GeneratorBlock

`generator_random_mode_produces_packets`
Runs the generator on a thread for 10 ms at T=100 µs. Verifies at least 20 packets appear in the queue — confirms the timing loop is actually producing output.

`generator_csv_mode_exact_values_and_order`
Runs the generator synchronously with a known CSV. Verifies exact v1/v2/row/col for all 4 packets and that the queue is empty afterwards — no extra packets, no missing packets.

`generator_csv_mode_no_extra_packets`
Single-row, 2-column CSV. Verifies exactly 1 packet is produced. Catches any loop-continuation bug that would emit padding or phantom packets after EOF.

`generator_stop_exits_within_grace_period`
Calls stop() after 2 ms and joins the thread. Asserts the whole thing finishes within 200 ms. Catches a hung spin loop or blocked sleep that ignores the stop flag.

`generator_stop_before_run_exits_immediately`
Sets stop() before calling run(). Verifies run() returns in under 10 ms — it should check the flag on the first iteration and exit without producing any packets.

`generator_back_pressure_handled`
Uses a tiny 4-slot queue with a very fast T=1 µs generator. Drains slowly for 5 ms. Verifies the generator doesn't deadlock or crash under sustained back-pressure — the drop-on-deadline path is exercised.

`generator_timing_within_tolerance`
Measures 20 consecutive inter-packet intervals at T=500 µs. Asserts each is within [0.5T, 4T]. Catches sleep drift, spin-wait failures, or gross scheduling problems that would make the pipeline miss its timing budget.
