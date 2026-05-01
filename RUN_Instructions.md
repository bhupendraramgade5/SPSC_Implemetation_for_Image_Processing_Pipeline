# CynLr Pipeline — Build & Run Instructions

Open a **x64 Native Tools Command Prompt for VS** and navigate to the project root:

```bat
cd C:\Users\bhupe\Desktop\CynLr
```

All commands below are run from this directory.

---

## Step 1 — Configure

Run once. Only re-run if you change `CMakeLists.txt` or `CMakePresets.json`.

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

For debug:
```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

For PERF instrumentation:
```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCYNLR_PERF=ON
```

---

## Step 2 — Build

```bat
cmake --build build --target CynLr_multiThread
cmake --build build --target CynLr_Linear
cmake --build build --target test_generator
cmake --build build --target test_filter
```

Build everything at once:
```bat
cmake --build build
```

Executables land in:
```
build\bin\
```

---

## Step 3 — Run

### Threaded pipeline

```bat
build\bin\CynLr_multiThread.exe --config=src\config.cfg
```

### Linear pipeline

```bat
build\bin\CynLr_Linear.exe --config=src\config.cfg
```

### CSV mode

```bat
build\bin\CynLr_multiThread.exe --config=src\config.cfg --mode=csv
build\bin\CynLr_Linear.exe      --config=src\config.cfg --mode=csv
```

### Unit tests

```bat
build\bin\test_generator.exe
build\bin\test_filter.exe
```

---

## config.cfg Reference

Located at `src\config.cfg`. Edit before running.

| Key | Type | Description |
|-----|------|-------------|
| `m` | int | Number of columns (must be even, ≥ 2) |
| `T` | int (ns) | Cycle time in nanoseconds (≥ 500) |
| `threshold` | int 0–255 | Binarisation threshold TV |
| `mode` | `random` / `csv` | Data source mode |
| `input_file` | path | CSV input (required when `mode=csv`) |
| `run_duration_ms` | int | Stop after N milliseconds (0 = unlimited) |
| `max_rows` | int | Stop after N rows (0 = unlimited) |
| `boundary_policy` | `replicate` / `zero_pad` | Row edge padding strategy |
| `write_output` | `true` / `false` | Write filtered output to CSV |
| `output_file` | path | Output CSV path (default `output.csv`) |
| `kernel` | comma-separated floats | Convolution kernel (must be odd count) |

---

## CLI Overrides

CLI flags override anything in `config.cfg`.

```bat
build\bin\CynLr_multiThread.exe --config=src\config.cfg --mode=csv
build\bin\CynLr_multiThread.exe --config=src\config.cfg --mode=random
build\bin\CynLr_multiThread.exe --config=src\config.cfg --duration=5000
build\bin\CynLr_multiThread.exe --config=src\config.cfg --max-rows=500
build\bin\CynLr_multiThread.exe --config=src\config.cfg --boundary=zero_pad
build\bin\CynLr_multiThread.exe --config=src\config.cfg --write-output --output-file=results.csv
```

---

## Troubleshooting

**config file not found** — always pass `--config=src\config.cfg` from the `CynLr` root.

**`m` must be even** — set `m` to an even number ≥ 2 in `config.cfg`.

**CSV row mismatch** — CSV must have exactly `m` values per row, or set `csv_mismatch_policy=truncate`.

**Dropped packets warning** — `T` is too small. Increase `T` or reduce `m`.

**PERF stats not printed** — reconfigure with `-DCYNLR_PERF=ON` and rebuild.