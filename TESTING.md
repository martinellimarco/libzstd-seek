# Testing

## Overview

The test suite exercises libzstd-seek across four dimensions:

| Dimension         | Tool                           | What it catches                                       |
|-------------------|--------------------------------|-------------------------------------------------------|
| **Correctness**   | round-trip + byte comparison   | decompression produces bit-identical output           |
| **Seek accuracy** | random seek/read verification  | every byte at every position matches expected content |
| **Memory safety** | AddressSanitizer + UBSanitizer | buffer overflows, use-after-free, undefined behaviour |
| **Code coverage** | LLVM coverage + llvm-cov       | dead or untested code paths                           |

~50 tests in total: 10 round-trip, ~25 API, and ~8 error-path tests.
All three build configurations run the same test suite.

---

## Prerequisites

| Dependency                         | Required for         | macOS                             | Linux                     |
|------------------------------------|----------------------|-----------------------------------|---------------------------|
| `cmake ≥ 3.16`                     | all builds           | `brew install cmake`              | `apt install cmake`       |
| `libzstd-dev`                      | all builds           | `brew install zstd`               | `apt install libzstd-dev` |
| LLVM (`llvm-cov`, `llvm-profdata`) | coverage report only | `brew install llvm`               | `apt install llvm`        |

---

## Quick start

```bash
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure
```

---

## Build configurations

### Debug (correctness)

Builds the library and test helpers without any instrumentation.
Use this for day-to-day development.

```bash
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure -j4
```

### ASAN + UBSan (memory safety)

Compiles with `-fsanitize=address,undefined`. Any memory violation or
undefined behaviour causes the affected test to fail with a detailed report
on stderr. On Linux, LeakSanitizer is also enabled; on macOS Apple Silicon
it is omitted as LSAN is not supported there.

```bash
cmake -B build_asan -DBUILD_TESTS=ON -DSANITIZE=ON
cmake --build build_asan
cd build_asan && ctest --output-on-failure -j4
```

### Coverage (LLVM)

Compiles with `-fprofile-instr-generate -fcoverage-mapping` and emits one
`.profraw` file per test run. After all tests complete, `test_coverage.sh`
merges the profiles and generates an HTML report.

```bash
cmake -B build_cov -DBUILD_TESTS=ON -DCOVERAGE=ON
cmake --build build_cov
cd build_cov && ctest --output-on-failure -j4

# Generate the HTML report (opens automatically on macOS)
bash ../tests/test_coverage.sh ../build_cov
# Report: tests/coverage/html/index.html
```

> **Note — LLVM path on macOS**
> `test_coverage.sh` looks for `llvm-cov` and `llvm-profdata` first in
> `/opt/homebrew/opt/llvm/bin` (Apple Silicon Homebrew) and falls back to
> the system `PATH`.

---

## Test categories

### Round-trip tests

| Test                       | Frames | Frame size | Flags                    | What is covered                            |
|----------------------------|--------|------------|--------------------------|--------------------------------------------|
| `rt_4frames_seekable`      | 4      | 1024       | `--seekable`             | seekable format footer parsing             |
| `rt_4frames_no_seekable`   | 4      | 1024       | —                        | frame header scanning                      |
| `rt_100frames_seekable`    | 100    | 1000       | `--seekable`             | large jump table                           |
| `rt_100frames_no_seekable` | 100    | 1000       | —                        | frame header scanning at scale             |
| `rt_single_frame`          | 1      | 65536      | —                        | single-frame edge case                     |
| `rt_single_frame_seekable` | 1      | 65536      | `--seekable`             | single-frame with seekable footer          |
| `rt_large_frames`          | 10     | 1 MB       | —                        | large frame decompression                  |
| `rt_no_content_size`       | 4      | 1024       | `--no-content-size`      | full decompression fallback path           |
| `rt_vary_size`             | 10     | varies     | `--seekable --vary-size` | non-uniform frame sizes                    |
| `rt_seekable_checksum`     | 4      | 1024       | `--seekable --checksum`  | seekable format with checksum entries      |

### API tests — context creation

| Test                     | API variant                                  |
|--------------------------|----------------------------------------------|
| `api_create_file`        | `ZSTDSeek_createFromFile`                    |
| `api_create_file_no_jt`  | `createFromFileWithoutJumpTable` + init      |
| `api_create_buffer`      | `ZSTDSeek_create` (from memory buffer)       |
| `api_create_buffer_no_jt`| `createWithoutJumpTable` + init              |
| `api_create_fd`          | `createFromFileDescriptor` + fileno check    |
| `api_create_fd_no_jt`    | `createFromFileDescriptorWithoutJumpTable`   |

### API tests — seek operations

| Test                   | What is covered                              |
|------------------------|----------------------------------------------|
| `api_seek_set_seq`     | SEEK_SET to every position, forward          |
| `api_seek_set_backward`| SEEK_SET from end to beginning               |
| `api_seek_cur_forward` | SEEK_CUR relative seeking forward            |
| `api_seek_end`         | SEEK_END with offset 0 and -1                |
| `api_seek_random`      | 10,000 random SEEK_SET + verify              |

### API tests — jump table

| Test                 | What is covered                                       |
|----------------------|-------------------------------------------------------|
| `api_jt_auto`        | auto-initialized jump table, frame count, length      |
| `api_jt_progressive` | `initializeJumpTableUpUntilPos` incremental init      |
| `api_jt_new_free`    | standalone `newJumpTable`/`addRecord`/`freeJumpTable` |

### API tests — seekable format

| Test                    | What is covered                                    |
|-------------------------|----------------------------------------------------|
| `api_seekable_basic`    | file with seekable footer: fast init + read/seek   |
| `api_seekable_checksum` | seekable format with checksum entries              |
| `api_seekable_vs_scan`  | seekable vs non-seekable produce identical results |

### API tests — info and read patterns

| Test                    | What is covered                              |
|-------------------------|----------------------------------------------|
| `api_file_size`         | `uncompressedFileSize`                       |
| `api_last_known_size`   | `lastKnownUncompressedFileSize` progression  |
| `api_frame_count`       | `getNumberOfFrames`                          |
| `api_is_multiframe`     | `isMultiframe` for multi and single frame    |
| `api_fileno`            | `fileno` returns valid fd                    |
| `api_compressed_tell`   | `compressedTell` coherence                   |
| `api_read_byte_by_byte` | sequential 1-byte reads                      |
| `api_read_chunks`       | chunked reads with verification              |
| `api_frame_boundary`    | read spanning frame boundary                 |
| `api_single_frame`      | single-frame file operations                 |
| `api_large_read`        | read buffer larger than file                 |

### Error paths

| Test                      | What is covered                                  |
|---------------------------|--------------------------------------------------|
| `err_null`                | all API functions with NULL arguments            |
| `err_empty_file`          | zero-byte file and zero-length buffer            |
| `err_truncated`           | truncated zstd file                              |
| `err_invalid_format`      | non-zstd file (rejected by `createFromFile`)     |
| `err_seek_negative`       | `SEEK_SET` with negative offset → `ERR_NEGATIVE` |
| `err_seek_beyond`         | seek past EOF → `ERR_BEYOND_END_SEEK`            |
| `err_seek_invalid_origin` | invalid origin value → `-1`                      |
| `err_read_past_eof`       | read at EOF returns short count then 0           |

---

## Coverage results

Current numbers measured on macOS Apple Silicon. Re-run
`tests/test_coverage.sh` after changes to get exact figures.

### What is not covered (and why)

- **Big-endian path in `ZSTDSeek_fromLE32`** — x86-64 and ARM64 are always little-endian.
- **`mmap()` failure** — requires OS-level fault injection.
- **`malloc` failure paths** — `ZSTDSeek_newJumpTable`, `ZSTDSeek_createWithoutJumpTable` OOM guards require memory exhaustion.
- **`stat()` failure in `createFromFile`** — requires filesystem-level fault injection.
- **Decompression error inside `initializeJumpTableUpUntilPos`** — requires corrupted frame that passes `ZSTD_findFrameCompressedSize` but fails `ZSTD_decompressStream`.

These are defensive guards, not untested logic.
