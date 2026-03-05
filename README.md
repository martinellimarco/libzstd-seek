[![AUR version](https://img.shields.io/aur/version/libzstd-seek)](https://aur.archlinux.org/packages/libzstd-seek/)

# libzstd-seek

This is a library to read [zstd](https://github.com/facebook/zstd) files at arbitraty positions.

The library has 3 main methods that mimic fread, fseek and fteel.

The usage is pretty simple.

You first create a new `ZSTDSeek_Context` from a file or a buffer in memory.

You then can use `ZSTDSeek_read`, `ZSTDSeek_seek` or `ZSTDSeek_tell` as you please to read uncompressed data.

In the end remember to free the context with `ZSTDSeek_free`.

This library can now decode the skiptable of the [seekable format](https://github.com/facebook/zstd/blob/dev/contrib/seekable_format/zstd_seekable_compression_format.md).

## Compile

```
mkdir build
cd build
cmake ..
make
```

If you want to get debug messages then `#define ZSTD_SEEK_DEBUG 1`

## Testing

The test suite (~50 tests) is built into the project and covers correctness,
seek accuracy, memory safety (ASAN + UBSAN), and code coverage (LLVM).

```bash
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure
```

See [TESTING.md](TESTING.md) for full documentation (build configurations,
test categories, coverage results).

## Fuzzing

Two [libFuzzer](https://llvm.org/docs/LibFuzzer.html) harnesses exercise the
decompression and seekable format parsing code paths.

See [FUZZING.md](FUZZING.md) for setup and usage instructions.

## Legacy tests

The original test suite is in a separate project,
[libzstd-seek-tests](https://github.com/martinellimarco/libzstd-seek-tests).

## How does it work?

The `ZSTDSeek_Context` holds a jump table that can be used for constant-time random access at zstd frame granularity (frames can be decompressed individually without inter-dependency).
`ZSTDSeek_initializeJumpTable` produces the jump table by:
* First, look for and validating a skiptable of the seekable format. If it looks good, it is used directly.
* If there is no such skiptable or if it is malformed (e.g. does not match the size of the file), we try to use `ZSTD_getFrameContentSize` to build a jump table in linear time (relative to the number of frames).
* If `ZSTD_getFrameContentSize` fails (happens when the optional "decompressed size" field of a frame is not set), we have to decompress the frame and find the size ourselves. This will be slow.

If you use the more advanced APIs you can fill in the jump table yourself or only request that the jump table be filled up to a target decompressed size when the skiptable is not present.

## Licensing

This source code is licensed under both the MIT license (found in the LICENSE file) and the GPLv3 (found in the COPYING file).

You may select, at your option, one of the above-listed licenses.
