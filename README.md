[![License](https://img.shields.io/badge/license-GPLv3-green.svg)](https://github.com/martinellimarco/libzstd-seek/blob/main/LICENSE)
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

If you want to get debug messages then `#define _ZSTD_SEEK_DEBUG_ 1`

## Tests

Tests are in a separate project, [libzstd-seek-tests](https://github.com/martinellimarco/libzstd-seek-tests).

## Licensing

This source code is licensed under both the MIT license (found in the LICENSE file) and the GPLv3 (found in the COPYING file).

You may select, at your option, one of the above-listed licenses.
