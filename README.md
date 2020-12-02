# libzstd-seek

This is a library to read [zstd](https://github.com/facebook/zstd) files at arbitraty positions.

The library has 3 main methods that mimic fread, fseek and fteel.

The usage is pretty simple.

You first create a new `ZSTDSeek_Context` from a file or a buffer in memory.

You then can use `ZSTDSeek_read`, `ZSTDSeek_seek` or `ZSTDSeek_tell` as you please to read uncompressed data.

In the end remember to free the context with `ZSTDSeek_free`. It's that easy.

## Compile

```
mkdir build
cd build
cmake ..
make
```

If you want debug messages then `#define _ZSTD_SEEK_DEBUG_ 1`

## Tests

Tests are in a separate project, [libzstd-seek-tests](https://github.com/martinellimarco/libzstd-seek-tests).

## Licensing

See LICENSE