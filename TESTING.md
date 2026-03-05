# Testing

## Overview

The test suite exercises libzstd-seek across four dimensions:

| Dimension         | Tool                           | What it catches                                       |
|-------------------|--------------------------------|-------------------------------------------------------|
| **Correctness**   | round-trip + byte comparison   | decompression produces bit-identical output           |
| **Seek accuracy** | random seek/read verification  | every byte at every position matches expected content |
| **Reference**     | library vs `zstd` CLI          | output identical to the reference zstd implementation |
| **Memory safety** | AddressSanitizer + UBSanitizer | buffer overflows, use-after-free, undefined behaviour |
| **Static analysis**| 5 analysers (see below)        | null derefs, leaks, type bugs, dead code             |
| **Code coverage** | LLVM coverage + llvm-cov       | dead or untested code paths                           |

77 tests in total: 11 round-trip, 41 API, 12 error-path, and 13 reference comparison tests.
All build configurations run the same test suite.

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

| Test                            | Frames | Frame size | Flags                    | What is covered                            |
|---------------------------------|--------|------------|--------------------------|--------------------------------------------|
| `rt_4frames_seekable`           | 4      | 1024       | `--seekable`             | seekable format footer parsing             |
| `rt_4frames_no_seekable`        | 4      | 1024       | —                        | frame header scanning                      |
| `rt_100frames_seekable`         | 100    | 1000       | `--seekable`             | large jump table                           |
| `rt_100frames_no_seekable`      | 100    | 1000       | —                        | frame header scanning at scale             |
| `rt_single_frame`               | 1      | 65536      | —                        | single-frame edge case                     |
| `rt_single_frame_seekable`      | 1      | 65536      | `--seekable`             | single-frame with seekable footer          |
| `rt_large_frames`               | 10     | 1 MB       | —                        | large frame decompression                  |
| `rt_no_content_size`            | 4      | 1024       | `--no-content-size`      | full decompression fallback path           |
| `rt_vary_size`                  | 10     | varies     | `--seekable --vary-size` | non-uniform frame sizes                    |
| `rt_seekable_checksum`          | 4      | 1024       | `--seekable --checksum`  | seekable format with checksum entries      |
| `rt_100frames_no_content_size`  | 100    | 1000       | `--no-content-size`      | slow path at scale (full decompression)    |

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

| Test                     | What is covered                                          |
|--------------------------|----------------------------------------------------------|
| `api_seek_set_seq`       | SEEK_SET to every position, forward                      |
| `api_seek_set_backward`  | SEEK_SET from end to beginning                           |
| `api_seek_cur_forward`   | SEEK_CUR relative seeking forward                        |
| `api_seek_cur_backward`  | SEEK_CUR with negative offset, backward traversal        |
| `api_seek_end`           | SEEK_END with offset 0 and -1                            |
| `api_seek_random`        | 10,000 random ops alternating SEEK_SET/CUR/END + verify  |
| `api_seek_out_of_file`   | 9 boundary cases (SET/CUR/END) with exact error codes    |

### API tests — jump table

| Test                       | What is covered                                             |
|----------------------------|-------------------------------------------------------------|
| `api_jt_auto`              | auto-init JT, frame count, record monotonicity, file size   |
| `api_jt_progressive`       | `initializeJumpTableUpUntilPos` incremental init            |
| `api_jt_new_free`          | standalone `newJumpTable`/`addRecord`/`freeJumpTable`       |
| `api_jt_manual`            | manual record insertion on real file, verify lengths + size |
| `api_jt_progressive_reads` | JT built incrementally via reads crossing frame boundaries  |

### API tests — seekable format

| Test                     | What is covered                                     |
|--------------------------|-----------------------------------------------------|
| `api_seekable_basic`     | file with seekable footer: fast init + read/seek    |
| `api_seekable_checksum`  | seekable format with checksum entries               |
| `api_seekable_vs_scan`   | seekable vs non-seekable produce identical results  |
| `api_seekable_malformed` | 4 corrupted footers: reserved bits, bad magic, size, numFrames overflow |

### API tests — info and read patterns

| Test                       | What is covered                                                  |
|----------------------------|------------------------------------------------------------------|
| `api_file_size`            | `uncompressedFileSize`                                           |
| `api_last_known_size`      | `lastKnownUncompressedFileSize` progression                      |
| `api_frame_count`          | `getNumberOfFrames`                                              |
| `api_is_multiframe`        | `isMultiframe` for multi and single frame                        |
| `api_fileno`               | `fileno` returns valid fd                                        |
| `api_compressed_tell`      | `compressedTell` coherence                                       |
| `api_compressed_tell_mono` | `compressedTell` never decreases during 10-byte sequential reads |
| `api_compressed_tell_seek` | `compressedTell` matches JT at frame boundaries after seek       |
| `api_seek_forward_large`   | `seek(+500, SEEK_CUR)` within frame, data verified vs raw        |
| `api_read_byte_by_byte`    | sequential 1-byte reads                                          |
| `api_read_chunks`          | chunked reads with verification                                  |
| `api_frame_boundary`       | read spanning frame boundary                                     |
| `api_single_frame`         | single-frame file operations                                     |
| `api_large_read`           | read buffer larger than file                                     |
| `api_read_too_much`        | request 2× file size from pos 0, short read                      |
| `api_seek_cur_zero`        | `seek(0, SEEK_CUR)` no-op at start/mid/eof                       |
| `api_seek_to_same_pos`     | seek to current position (non-zero) twice                        |
| `api_fileno_buffer`        | `fileno` returns -1 for buffer-created context                   |
| `api_read_zero_bytes`      | `read(buf, 0, ctx)` returns 0 at start, mid, and EOF            |

### Error paths

| Test                      | What is covered                                   |
|---------------------------|---------------------------------------------------|
| `err_null`                | all API functions with NULL arguments             |
| `err_empty_file`          | zero-byte file and zero-length buffer             |
| `err_truncated`           | truncated zstd file                               |
| `err_invalid_format`      | non-zstd file (rejected by `createFromFile`)      |
| `err_seek_negative`       | `SEEK_SET` with negative offset → `ERR_NEGATIVE`  |
| `err_seek_beyond`         | seek past EOF → `ERR_BEYOND_END_SEEK`             |
| `err_seek_invalid_origin` | invalid origin value → `-1`                       |
| `err_read_past_eof`       | read at EOF returns short count then 0            |
| `err_corrupted_header`    | zstd with corrupted magic number → create fails   |
| `err_mixed_format`        | valid ZSTD + garbage, create with valid-only size |
| `err_corrupted_frame_data`| corrupt 2nd frame payload → decompression error   |
| `err_seektable_bad_offsets`| seektable dc=0xFFFFFFFF → fallback to frame scan |

---

## Coverage results

Current numbers for `zstd-seek.c` measured on macOS Apple Silicon with all
77 tests (Debug, LLVM coverage build):

| Metric        | Value         |
|---------------|---------------|
| **Functions** | 27/27 (100%)  |
| **Lines**     | 456/512 (89%) |
| **Branches**  | 215/274 (78%) |

Re-run `tests/test_coverage.sh` after changes to get exact figures.

### What is not covered (and why)

- **Big-endian path in `ZSTDSeek_fromLE32`** — x86-64 and ARM64 are always little-endian.
  Compile-time detection via `__BYTE_ORDER__` / `_WIN32` eliminates the runtime
  fallback on GCC/Clang/MSVC.
- **`mmap()` failure** — requires OS-level fault injection.
- **`malloc` failure paths** — `ZSTDSeek_newJumpTable`, `ZSTDSeek_createWithoutJumpTable` OOM guards require memory exhaustion.
- **`stat()` failure in `createFromFile`** — requires filesystem-level fault injection.
- **`SIZE_MAX` guard in `createFromFile` / `createFromFileDescriptor`** — requires a file larger than `SIZE_MAX` on a 32-bit system with large file support.
- **`size_t` overflow guards in frame scan** (`compressedPos`, `uncompressedPos`, `frameContentSize` probing) — require accumulated positions exceeding `SIZE_MAX`, which is only possible on 32-bit with very large streams.

These are defensive guards or platform-specific paths, not untested logic.

---

## Static analysis

Five static analysers have been run against the codebase (library + examples).
All pass with zero findings.

### Clang Static Analyzer (scan-build)

Part of LLVM. Wraps the build and injects analysis passes.

```bash
brew install llvm            # provides scan-build
scan-build cmake -B build_sa -DCMAKE_BUILD_TYPE=Debug
scan-build cmake --build build_sa
```

### clang-tidy

LLVM linter/analyser. Requires a `compile_commands.json`.

```bash
cmake -B build_sa -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build_sa

clang-tidy -p build_sa \
  --checks='clang-analyzer-*,bugprone-*,cert-*,misc-*,performance-*' \
  zstd-seek.c
```

### clang --analyze (Apple Clang)

Ships with Xcode. No extra installation needed on macOS.

```bash
clang --analyze \
  -Xanalyzer -analyzer-checker=core \
  -Xanalyzer -analyzer-checker=security \
  -Xanalyzer -analyzer-checker=unix \
  -Xanalyzer -analyzer-checker=deadcode \
  -Xanalyzer -analyzer-output=text \
  -I/opt/homebrew/include zstd-seek.c
```

### cppcheck

Independent C/C++ analyser.

```bash
brew install cppcheck        # or apt install cppcheck

cppcheck --enable=all --std=c99 \
  --suppress=missingIncludeSystem \
  --suppress=unusedFunction \
  -I/opt/homebrew/include \
  zstd-seek.c
```

`--suppress=unusedFunction` is needed because the public API functions are
called by client code, not within the single translation unit.

### Facebook Infer

Download from <https://fbinfer.com> and add to `PATH`.

```bash
cmake -B build_sa -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build_sa

infer run --compilation-database build_sa/compile_commands.json
```

Infer detects null dereferences, resource leaks, buffer overruns, dead stores,
and uninitialized values.

---

## Fuzzing

See [FUZZING.md](FUZZING.md) for the libFuzzer harness documentation.
