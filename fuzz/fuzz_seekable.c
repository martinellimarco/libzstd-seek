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
 * Build:
 *   CC=clang cmake -B build_fuzz -DFUZZ=ON
 *   cmake --build build_fuzz
 *
 * Run:
 *   ./build_fuzz/fuzz/fuzz_seekable corpus/ -max_len=4096 -timeout=5
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

    /* Try to create a context — this will invoke the seekable format
     * parser if the last bytes look like a valid footer. */
    ZSTDSeek_Context *ctx = ZSTDSeek_create(buf, total);
    if (ctx) {
        /* If creation succeeded, exercise basic operations */
        char read_buf[64];
        ZSTDSeek_read(read_buf, sizeof(read_buf), ctx);
        ZSTDSeek_seek(ctx, 0, SEEK_SET);
        ZSTDSeek_uncompressedFileSize(ctx);
        ZSTDSeek_getNumberOfFrames(ctx);

        ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
        if (jt) {
            /* Access jump table records if available */
            for (uint64_t i = 0; i < jt->length && i < 1000; i++) {
                (void)jt->records[i].compressedPos;
                (void)jt->records[i].uncompressedPos;
            }
        }

        ZSTDSeek_free(ctx);
    }

    free(buf);
    return 0;
}
