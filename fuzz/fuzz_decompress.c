// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * fuzz_decompress — libFuzzer harness for libzstd-seek.
 *
 * Feeds random data to ZSTDSeek_create(), then performs a random sequence
 * of seek and read operations.  This exercises all code paths:
 * - Jump table initialization (seekable format, frame headers, decompression)
 * - Binary search for frame lookup
 * - Decompression and frame boundary handling
 * - Error handling for malformed data
 *
 * Unlike t2sz's fuzzing harnesses, no setjmp/longjmp is needed because
 * libzstd-seek returns errors via return values rather than calling exit().
 *
 * Build:
 *   CC=clang cmake -B build_fuzz -DFUZZ=ON
 *   cmake --build build_fuzz
 *
 * Run:
 *   ./build_fuzz/fuzz/fuzz_decompress corpus/ -max_len=65536 -timeout=10
 */

#include "zstd-seek.h"
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Need at least 4 bytes for ZSTD magic number check */
    if (size < 4) return 0;

    /* Try to create a context from the fuzzer's data.
     * ZSTDSeek_create takes a non-const void*, but only reads from it
     * (via mmap-like usage). Cast is safe. */
    ZSTDSeek_Context *ctx = ZSTDSeek_create((void *)data, size);
    if (!ctx) return 0; /* Invalid data — expected */

    /* Get uncompressed file size (triggers full jump table init) */
    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);

    if (file_size > 0 && size >= 8) {
        char buf[4096];

        /* Use bytes from the fuzz input to drive seek positions.
         * This ensures the fuzzer can discover interesting seek patterns. */
        size_t max_ops = size / 4;
        if (max_ops > 64) max_ops = 64;

        for (size_t i = 0; i < max_ops; i++) {
            uint32_t raw_pos;
            memcpy(&raw_pos, data + (i * 4) % size, 4);
            long pos = (long)(raw_pos % (file_size + 1));

            int rc = ZSTDSeek_seek(ctx, pos, SEEK_SET);
            if (rc == 0) {
                ZSTDSeek_read(buf, sizeof(buf), ctx);
            }
        }

        /* Also exercise SEEK_CUR and SEEK_END */
        ZSTDSeek_seek(ctx, 0, SEEK_SET);
        ZSTDSeek_seek(ctx, 1, SEEK_CUR);
        ZSTDSeek_seek(ctx, 0, SEEK_END);
        ZSTDSeek_seek(ctx, -1, SEEK_END);

        /* Exercise info functions */
        ZSTDSeek_tell(ctx);
        ZSTDSeek_compressedTell(ctx);
        ZSTDSeek_getNumberOfFrames(ctx);
        ZSTDSeek_isMultiframe(ctx);
        ZSTDSeek_lastKnownUncompressedFileSize(ctx);
        ZSTDSeek_fileno(ctx);

        /* Exercise jump table access */
        ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
        (void)jt; /* just verify no crash */
    }

    ZSTDSeek_free(ctx);
    return 0;
}
