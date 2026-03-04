// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * fuzz_seekable — libFuzzer harness focused on seekable format parsing.
 *
 * Generates a valid minimal zstd frame as a fixed prefix, then appends
 * the fuzzer's data as a potential seekable format skiptable/footer.
 * This focuses mutations on the seekable format parser code path:
 * - Skippable frame magic validation
 * - Seek table footer parsing (numFrames, sfd, magic)
 * - Entry parsing (compressed/uncompressed sizes, checksums)
 * - Malformed skiptable handling (graceful fallback to frame scan)
 *
 * Also exercises the WithoutJumpTable → initializeJumpTable path to test
 * seekable format detection in a different creation flow.
 *
 * Build:
 *   CC=clang cmake -B build_fuzz -DFUZZ=ON
 *   cmake --build build_fuzz
 *
 * Run:
 *   ./build_fuzz/fuzz/fuzz_seekable corpus/ -dict=../fuzz/zstd_seek.dict \
 *       -max_len=4096 -timeout=5
 */

#include "zstd-seek.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

/* A pre-compressed minimal zstd frame containing "Hello" (5 bytes).
 * Generated with: echo -n Hello | zstd -1 --no-check | xxd -i
 * This is a valid single-frame zstd file that the library can parse. */
static const uint8_t VALID_FRAME[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x48, 0x29, 0x00,
    0x00, 0x48, 0x65, 0x6c, 0x6c, 0x6f
};
static const size_t VALID_FRAME_SIZE = sizeof(VALID_FRAME);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;

    /* Construct: valid_frame + fuzz_data
     * The fuzz data is appended as a potential seekable format footer.
     * The seekable format parser looks at the last 9 bytes for the footer
     * and works backwards to find the skiptable. */
    size_t total = VALID_FRAME_SIZE + size;
    uint8_t *buf = malloc(total);
    if (!buf) return 0;

    memcpy(buf, VALID_FRAME, VALID_FRAME_SIZE);
    memcpy(buf + VALID_FRAME_SIZE, data, size);

    /* ── Path A: ZSTDSeek_create (auto JT init) ──────────────────────── */
    ZSTDSeek_Context *ctx = ZSTDSeek_create(buf, total);
    if (ctx) {
        /* Exercise basic operations */
        char read_buf[64];
        ZSTDSeek_read(read_buf, sizeof(read_buf), ctx);
        ZSTDSeek_seek(ctx, 0, SEEK_SET);
        ZSTDSeek_read(read_buf, 1, ctx);  /* single byte read */
        ZSTDSeek_uncompressedFileSize(ctx);
        ZSTDSeek_getNumberOfFrames(ctx);
        ZSTDSeek_isMultiframe(ctx);
        ZSTDSeek_tell(ctx);
        ZSTDSeek_compressedTell(ctx);
        ZSTDSeek_lastKnownUncompressedFileSize(ctx);
        ZSTDSeek_fileno(ctx);

        /* Try seeking */
        size_t fs = ZSTDSeek_uncompressedFileSize(ctx);
        if (fs > 0) {
            ZSTDSeek_seek(ctx, 0, SEEK_END);
            ZSTDSeek_seek(ctx, -1, SEEK_END);
            ZSTDSeek_read(read_buf, 1, ctx);
            ZSTDSeek_seek(ctx, 0, SEEK_CUR); /* no-op */
        }

        ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
        if (jt) {
            for (uint64_t i = 0; i < jt->length && i < 1000; i++) {
                (void)jt->records[i].compressedPos;
                (void)jt->records[i].uncompressedPos;
            }
        }

        ZSTDSeek_free(ctx);
    }

    /* ── Path B: createWithoutJumpTable → initializeJumpTable ────────── */
    /* This exercises the seekable format detection via a different path:
     * the JT is not auto-initialized at creation, so initializeJumpTable
     * runs the seekable footer parsing from scratch. */
    ctx = ZSTDSeek_createWithoutJumpTable(buf, total);
    if (ctx) {
        /* JT should not be initialized yet */
        (void)ZSTDSeek_jumpTableIsInitialized(ctx);
        (void)ZSTDSeek_lastKnownUncompressedFileSize(ctx);

        /* Trigger partial init with fuzzer-controlled position */
        if (size >= 4) {
            uint32_t up_until;
            memcpy(&up_until, data, 4);
            ZSTDSeek_initializeJumpTableUpUntilPos(ctx, (size_t)(up_until % 1024));
        }

        (void)ZSTDSeek_jumpTableIsInitialized(ctx);
        (void)ZSTDSeek_lastKnownUncompressedFileSize(ctx);

        /* Full init */
        ZSTDSeek_initializeJumpTable(ctx);

        /* Exercise after full init */
        char read_buf[64];
        ZSTDSeek_seek(ctx, 0, SEEK_SET);
        ZSTDSeek_read(read_buf, sizeof(read_buf), ctx);
        ZSTDSeek_tell(ctx);

        ZSTDSeek_free(ctx);
    }

    free(buf);
    return 0;
}
