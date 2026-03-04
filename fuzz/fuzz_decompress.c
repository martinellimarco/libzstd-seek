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
 *   ./build_fuzz/fuzz/fuzz_decompress corpus/ -dict=../fuzz/zstd_seek.dict \
 *       -max_len=65536 -timeout=10
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

        /* Use bytes from the fuzz input to drive seek positions and modes.
         * This ensures the fuzzer can discover interesting seek patterns.
         * Each operation consumes 5 bytes: 4 for position, 1 for mode+readsize */
        size_t max_ops = size / 5;
        if (max_ops > 64) max_ops = 64;

        for (size_t i = 0; i < max_ops; i++) {
            size_t off = (i * 5) % size;
            uint32_t raw_pos;
            memcpy(&raw_pos, data + off, 4);

            /* Byte 5 selects: bits 0-1 = seek mode, bits 2-4 = read size */
            uint8_t control = (off + 4 < size) ? data[off + 4] : 0;
            unsigned mode = control & 0x03;
            unsigned read_shift = (control >> 2) & 0x07;
            size_t read_size = (size_t)1 << read_shift; /* 1, 2, 4, ..., 128 */
            if (read_size > sizeof(buf)) read_size = sizeof(buf);

            int rc;
            switch (mode) {
            case 0: { /* SEEK_SET */
                long pos = (long)(raw_pos % (file_size + 1));
                rc = ZSTDSeek_seek(ctx, pos, SEEK_SET);
                break;
            }
            case 1: { /* SEEK_CUR */
                long cur = ZSTDSeek_tell(ctx);
                long target = (long)(raw_pos % (file_size + 1));
                rc = ZSTDSeek_seek(ctx, target - cur, SEEK_CUR);
                break;
            }
            case 2: { /* SEEK_END */
                long neg_off = -((long)(raw_pos % (file_size + 1)));
                rc = ZSTDSeek_seek(ctx, neg_off, SEEK_END);
                break;
            }
            default: /* also SEEK_SET for mode==3 */
                rc = ZSTDSeek_seek(ctx, (long)(raw_pos % (file_size + 1)), SEEK_SET);
                break;
            }

            if (rc == 0) {
                ZSTDSeek_read(buf, read_size, ctx);
            }
        }

        /* Exercise edge-case seeks */
        ZSTDSeek_seek(ctx, 0, SEEK_SET);  /* beginning */
        ZSTDSeek_read(buf, 1, ctx);       /* single byte at start */
        ZSTDSeek_seek(ctx, 0, SEEK_CUR);  /* no-op (early return path) */
        ZSTDSeek_seek(ctx, 0, SEEK_END);  /* EOF */
        ZSTDSeek_seek(ctx, -1, SEEK_END); /* last byte */
        ZSTDSeek_read(buf, 1, ctx);       /* single byte at end */

        /* Exercise error paths (should not crash) */
        ZSTDSeek_seek(ctx, -1, SEEK_SET);              /* negative seek */
        ZSTDSeek_seek(ctx, (long)(file_size + 1), SEEK_SET); /* beyond end */
        ZSTDSeek_seek(ctx, 0, 99);                     /* invalid origin */

        /* Exercise info functions */
        ZSTDSeek_tell(ctx);
        ZSTDSeek_compressedTell(ctx);
        ZSTDSeek_getNumberOfFrames(ctx);
        ZSTDSeek_isMultiframe(ctx);
        ZSTDSeek_lastKnownUncompressedFileSize(ctx);
        ZSTDSeek_fileno(ctx);

        /* Exercise jump table access */
        ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
        if (jt) {
            for (uint64_t i = 0; i < jt->length && i < 1000; i++) {
                (void)jt->records[i].compressedPos;
                (void)jt->records[i].uncompressedPos;
            }
        }
    }

    ZSTDSeek_free(ctx);
    return 0;
}
