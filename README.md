[![AUR version](https://img.shields.io/aur/version/libzstd-seek)](https://aur.archlinux.org/packages/libzstd-seek/)
[![Build Status](https://github.com/martinellimarco/libzstd-seek/workflows/tests/badge.svg)](https://github.com/martinellimarco/libzstd-seek/actions)

# libzstd-seek

This is a library to read [zstd](https://github.com/facebook/zstd) files at arbitrary positions.

The library has 3 main methods that mimic `fread`, `fseek` and `ftell`.

The usage is pretty simple.

You first create a new `ZSTDSeek_Context` from a file or a buffer in memory.

You can then use `ZSTDSeek_read`, `ZSTDSeek_seek` or `ZSTDSeek_tell` as you please to read uncompressed data.

In the end remember to free the context with `ZSTDSeek_free`.

This library can now decode the skiptable of the [seekable format](https://github.com/facebook/zstd/blob/dev/contrib/seekable_format/zstd_seekable_compression_format.md).

## Building

### Prerequisites

| Dependency   | Debian / Ubuntu           | Arch              | macOS                |
|--------------|---------------------------|-------------------|----------------------|
| cmake ≥ 3.16 | `apt install cmake`       | `pacman -S cmake` | `brew install cmake` |
| libzstd      | `apt install libzstd-dev` | `pacman -S zstd`  | `brew install zstd`  |

### Quick start

```bash
cmake -B build
cmake --build build
```

This builds the static library `libzstd-seek.a` (or `.lib` on Windows).

### CMake options

| Option           | Default | Description                                                              |
|------------------|---------|--------------------------------------------------------------------------|
| `BUILD_EXAMPLES` | **OFF** | Build the example programs in `examples/`                                |
| `BUILD_TESTS`    | **OFF** | Build the test suite (see [TESTING.md](TESTING.md))                      |
| `SANITIZE`       | **OFF** | Enable AddressSanitizer + UndefinedBehaviorSanitizer                     |
| `COVERAGE`       | **OFF** | Enable LLVM coverage instrumentation (requires Clang)                    |
| `FUZZ`           | **OFF** | Build libFuzzer harnesses (requires Clang, see [FUZZING.md](FUZZING.md)) |
| `DEBUG_LOG`      | **OFF** | Enable debug messages on stderr                                          |

`SANITIZE`, `COVERAGE` and `FUZZ` are mutually exclusive — use separate build directories.

### Examples

```bash
# Build with examples and tests
cmake -B build -DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Build with debug logging
cmake -B build_dbg -DDEBUG_LOG=ON
cmake --build build_dbg
```

> **Note:** `DEBUG_LOG` has no effect on Windows because the `DEBUG()` macro relies
> on `flockfile()`/`funlockfile()` which are not available on that platform.

## Testing

The test suite is built into the project and covers correctness, seek accuracy, memory safety (ASAN + UBSAN), and code coverage (LLVM).

```bash
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure
```

See [TESTING.md](TESTING.md) for full documentation (build configurations, test categories, coverage results).

## Fuzzing

Three [libFuzzer](https://llvm.org/docs/LibFuzzer.html) harnesses exercise the decompression, seekable format parsing, and jump table code paths.

See [FUZZING.md](FUZZING.md) for setup and usage instructions.

## Legacy tests

The original test suite is in a separate project, [libzstd-seek-tests](https://github.com/martinellimarco/libzstd-seek-tests).

## How does it work?

The `ZSTDSeek_Context` holds a jump table that can be used for constant-time random access at zstd frame granularity (frames can be decompressed individually without inter-dependency).
`ZSTDSeek_initializeJumpTable` produces the jump table by:
* First, look for and validate a skiptable of the seekable format. If it looks good, it is used directly.
* If there is no such skiptable or if it is malformed (e.g. does not match the size of the file), we try to use `ZSTD_getFrameContentSize` to build a jump table in linear time (relative to the number of frames).
* If `ZSTD_getFrameContentSize` fails (happens when the optional "decompressed size" field of a frame is not set), we have to decompress the frame and find the size ourselves. This will be slow.

If you use the more advanced APIs, you can fill in the jump table yourself or only request that the jump table be filled up to a target decompressed size when the skiptable is not present.

## Licensing

This source code is licensed under both the MIT license (found in the LICENSE file) and the GPLv3 (found in the COPYING file).

You may select, at your option, one of the above-listed licenses.
