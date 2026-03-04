# Fuzzing

## Overview

The `fuzz/` directory contains [libFuzzer](https://llvm.org/docs/LibFuzzer.html)
harnesses for fuzz-testing libzstd-seek. Fuzzing complements the deterministic
test suite (~50 tests) by exploring the vast space of possible inputs that
manual tests cannot cover.

Two harnesses are provided:

| Harness           | Code path              | What it exercises                                                                                                  |
|-------------------|------------------------|--------------------------------------------------------------------------------------------------------------------|
| `fuzz_decompress` | general decompression  | `ZSTDSeek_create`, `ZSTDSeek_seek` (SET/CUR/END), `ZSTDSeek_read`, jump table initialization, info functions       |
| `fuzz_seekable`   | seekable format parser | skiptable footer parsing, malformed magic numbers, truncated entries, checksum flag handling, frame count mismatch |

Neither harness requires `setjmp`/`longjmp` — unlike CLI tools that call
`exit()` on errors, libzstd-seek returns error codes via its API. This makes
the harnesses simple and leak-free.

---

## Prerequisites

| Dependency    | Required                            | Install                                                      |
|---------------|-------------------------------------|--------------------------------------------------------------|
| Clang         | yes (libFuzzer is built into clang) | macOS: `brew install llvm`; Linux: `apt install clang`       |
| libzstd-dev   | yes                                 | macOS: `brew install zstd`; Linux: `apt install libzstd-dev` |
| cmake >= 3.16 | yes                                 | macOS: `brew install cmake`; Linux: `apt install cmake`      |

> **macOS note**: AppleClang (shipped with Xcode) does **not** include the
> libFuzzer runtime. You must use Homebrew LLVM clang instead. The build
> system automatically detects macOS and handles two platform-specific
> compatibility issues:
> 1. Links against Homebrew LLVM's libc++ (Apple's system libc++ is missing
>    symbols used by the libFuzzer runtime).
> 2. Excludes the `vptr` and `function` UBSAN checks, which generate
>    relocations incompatible with Apple's system linker. All other UBSAN
>    checks (signed/unsigned overflow, shift, null, alignment, etc.) remain
>    active.

---

## Quick start

```bash
# Build the fuzz harnesses
# Linux:
CC=clang cmake -B build_fuzz -DFUZZ=ON
cmake --build build_fuzz

# macOS (Homebrew LLVM):
CC=/opt/homebrew/opt/llvm/bin/clang cmake -B build_fuzz -DFUZZ=ON
cmake --build build_fuzz

# Generate the seed corpus
cmake --build build_fuzz --target fuzz_corpus

# Run the decompress harness (most important target)
cd build_fuzz/fuzz
./fuzz_decompress corpus_decompress/ -max_len=65536 -timeout=10 -detect_leaks=0

# Run the seekable format harness
./fuzz_seekable corpus_seekable/ -max_len=4096 -timeout=5 -detect_leaks=0
```

For multi-core fuzzing:

```bash
./fuzz_decompress corpus_decompress/ -max_len=65536 -timeout=10 -detect_leaks=0 -jobs=4 -workers=4
```

---

## Recommended flags

| Flag                 | Value     | Why                                                                                       |
|----------------------|-----------|-------------------------------------------------------------------------------------------|
| `-max_len`           | `65536`   | 64 KiB is enough for multi-frame archives; larger inputs waste cycles on zstd compression |
| `-timeout`           | `10`      | Kills iterations that hang (e.g., crafted frame sizes cause excessive decompression)      |
| `-detect_leaks`      | `0`       | Disables LeakSanitizer; avoids false positives on macOS Apple Silicon (LSAN unsupported)  |
| `-rss_limit_mb`      | `4096`    | Caps RSS to 4 GiB; prevents OOM on inputs with huge decompressed size fields              |
| `-jobs` / `-workers` | CPU cores | Parallel fuzzing across multiple processes                                                |

> **Note**: on some platforms the `-detect_leaks=0` flag alone may not
> suppress LSAN. If you still see leak reports, set the environment variable
> explicitly: `ASAN_OPTIONS=detect_leaks=0 ./fuzz_decompress ...`

---

## Reproducing and minimizing crashes

```bash
# Reproduce a crash
./fuzz_decompress crash-XXXXXXX

# Minimize the crash input to the smallest reproducer
./fuzz_decompress -minimize_crash=1 -exact_artifact_path=minimized.bin crash-XXXXXXX
```

---

## Converting a crash to a deterministic test

When the fuzzer finds a crash, the recommended workflow is:

1. Minimize the crash input (see above).
2. Copy the minimized file into `tests/` (e.g., `tests/crash_decompress_001.bin`).
3. Add a new test case in `tests/test_seek.c` (e.g., `error_crash_001`) that
   opens the crash file via `ZSTDSeek_create` and asserts the expected behavior
   (NULL return or correct error handling, no crash).
4. Register the test in `tests/CMakeLists.txt` and update `TESTING.md`.
5. Fix the bug in `zstd-seek.c`.
6. Verify: the crash reproducer now passes, and all ~50 tests still pass.

This ensures the bug never regresses.

---

## Architecture

### Harness design

libzstd-seek is a library with a clean API that returns error codes (NULL on
creation failure, -1 or error constants on seek failure, 0 on read failure).
It never calls `exit()` or `abort()` — all error paths are exercised simply by
calling the API and checking return values.

This means the harnesses are simple:

```c
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ZSTDSeek_Context *ctx = ZSTDSeek_create((void *)data, size);
    if (!ctx) return 0;      // invalid input — silently skip
    // ... exercise seek/read with fuzz-derived offsets ...
    ZSTDSeek_free(ctx);
    return 0;
}
```

No `setjmp`/`longjmp`, no `exit()` override, no `dlsym` tricks. The harness
is entirely self-contained.

### `fuzz_decompress` — general decompression

1. Creates a context from the raw fuzz input via `ZSTDSeek_create()`.
2. If creation succeeds, reads `uncompressedFileSize` and performs a series of
   random seek/read operations driven by bytes from the fuzz input itself.
3. Exercises `SEEK_SET`, `SEEK_CUR`, `SEEK_END`, and all info functions
   (`getNumberOfFrames`, `isMultiframe`, `compressedTell`, `fileno`).

### `fuzz_seekable` — seekable format parser

The seekable format parser is a particularly interesting target because it
interprets a binary skiptable appended to the compressed data. Malformed
skiptables could trigger out-of-bounds reads or integer overflows.

1. Prepends a valid minimal zstd frame (constant 14-byte compressed "Hello")
   to ensure the decompressor has at least one valid frame.
2. Appends the raw fuzz input as a potential seekable footer.
3. Calls `ZSTDSeek_create()` which invokes the skiptable parser.

This structure ensures the fuzzer focuses mutations on the footer/skiptable
bytes rather than wasting iterations on invalid zstd frame headers.

---

## Build options

The `-DFUZZ=ON` option is mutually exclusive with `-DSANITIZE=ON` and
`-DCOVERAGE=ON` because the fuzz build includes its own ASAN + UBSAN
instrumentation via `-fsanitize=fuzzer,address,undefined`.

On macOS, the build automatically adjusts to
`-fsanitize=fuzzer,address,undefined -fno-sanitize=vptr,function` to avoid
linker incompatibilities with Apple's `ld`. All C-relevant UBSAN checks remain
active; only the C++-specific `vptr` and `function` checks are excluded.
