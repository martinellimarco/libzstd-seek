// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * fuzz_jumptable — libFuzzer harness for manual jump table construction.
 *
 * Exercises the WithoutJumpTable + manual addJumpTableRecord path.
 * Also tests initializeJumpTableUpUntilPos with fuzzer-controlled positions.
 *
 * The input is split into two parts:
 *   1. The first portion is used as zstd data (with a valid prefix)
 *   2. The remaining bytes drive jump table record values and seek positions
 *
 * Build:
 *   CC=clang cmake -B build_fuzz -DFUZZ=ON
 *   cmake --build build_fuzz
 *
 * Run:
 *   ./build_fuzz/fuzz/fuzz_jumptable corpus/ -dict=../fuzz/zstd_seek.dict \
 *       -max_len=65536 -timeout=10
 */

#include "zstd-seek.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A pre-compressed minimal zstd frame containing "Hello" (5 bytes).
 * Same as fuzz_seekable uses. */
static const uint8_t VALID_FRAME[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x48, 0x29, 0x00,
    0x00, 0x48, 0x65, 0x6c, 0x6c, 0x6f
};
static const size_t VALID_FRAME_SIZE = sizeof(VALID_FRAME);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 8) return 0;

    /* ── Part A: standalone jump table lifecycle ──────────────────────── */
    {
        ZSTDSeek_JumpTable *jt = ZSTDSeek_newJumpTable();
        if (!jt) return 0;

        /* Use fuzzer bytes to add records */
        size_t num_records = size / 8;
        if (num_records > 256) num_records = 256;

        for (size_t i = 0; i < num_records; i++) {
            uint32_t comp, uncomp;
            memcpy(&comp, data + i * 8, 4);
            memcpy(&uncomp, data + i * 8 + 4, 4);
            ZSTDSeek_addJumpTableRecord(jt, (size_t)comp, (size_t)uncomp);
        }

        /* Exercise read-back of records */
        for (uint64_t i = 0; i < jt->length; i++) {
            (void)jt->records[i].compressedPos;
            (void)jt->records[i].uncompressedPos;
        }

        ZSTDSeek_freeJumpTable(jt);
    }

    /* ── Part B: createWithoutJumpTable + initializeJumpTableUpUntilPos ─ */
    {
        /* Build buffer: valid frame + (optional) more frames from fuzz data */
        size_t total = VALID_FRAME_SIZE + size;
        uint8_t *buf = malloc(total);
        if (!buf) return 0;

        memcpy(buf, VALID_FRAME, VALID_FRAME_SIZE);
        memcpy(buf + VALID_FRAME_SIZE, data, size);

        ZSTDSeek_Context *ctx = ZSTDSeek_createWithoutJumpTable(buf, total);
        if (ctx) {
            /* Jump table should not be initialized yet */
            (void)ZSTDSeek_jumpTableIsInitialized(ctx);

            /* Use first 4 bytes as upUntilPos */
            uint32_t up_until;
            memcpy(&up_until, data, 4);
            ZSTDSeek_initializeJumpTableUpUntilPos(ctx, (size_t)up_until);

            /* Check partial state */
            (void)ZSTDSeek_jumpTableIsInitialized(ctx);
            (void)ZSTDSeek_lastKnownUncompressedFileSize(ctx);

            ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
            if (jt) {
                for (uint64_t i = 0; i < jt->length && i < 1000; i++) {
                    (void)jt->records[i].compressedPos;
                    (void)jt->records[i].uncompressedPos;
                }
            }

            /* Now fully initialize */
            ZSTDSeek_initializeJumpTable(ctx);
            (void)ZSTDSeek_jumpTableIsInitialized(ctx);
            (void)ZSTDSeek_uncompressedFileSize(ctx);

            /* Try some reads and seeks */
            char read_buf[64];
            ZSTDSeek_seek(ctx, 0, SEEK_SET);
            ZSTDSeek_read(read_buf, sizeof(read_buf), ctx);
            ZSTDSeek_tell(ctx);
            ZSTDSeek_compressedTell(ctx);

            ZSTDSeek_free(ctx);
        }

        free(buf);
    }

    /* ── Part C: createFromBuffer without JT + manual records ───────── */
    {
        uint8_t *buf = malloc(VALID_FRAME_SIZE);
        if (!buf) return 0;
        memcpy(buf, VALID_FRAME, VALID_FRAME_SIZE);

        ZSTDSeek_Context *ctx = ZSTDSeek_createWithoutJumpTable(buf, VALID_FRAME_SIZE);
        if (ctx) {
            ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
            if (jt) {
                /* Add fuzzer-controlled records (monotonically increasing) */
                size_t num = size / 8;
                if (num > 64) num = 64;
                size_t cum_comp = 0, cum_uncomp = 0;
                for (size_t i = 0; i < num; i++) {
                    uint32_t comp, uncomp;
                    memcpy(&comp, data + i * 8, 4);
                    memcpy(&uncomp, data + i * 8 + 4, 4);
                    /* Build monotonically increasing positions */
                    cum_comp += comp % (VALID_FRAME_SIZE + 1);
                    cum_uncomp += uncomp % 256;
                    ZSTDSeek_addJumpTableRecord(jt, cum_comp, cum_uncomp);
                }
            }

            /* Try operations with the manually-constructed JT */
            char read_buf[64];
            ZSTDSeek_read(read_buf, sizeof(read_buf), ctx);
            (void)ZSTDSeek_tell(ctx);

            /* Seek with fuzzer-controlled position */
            if (size >= 4) {
                uint32_t pos;
                memcpy(&pos, data, 4);
                ZSTDSeek_seek(ctx, (long)(pos % 128), SEEK_SET);
                ZSTDSeek_read(read_buf, sizeof(read_buf), ctx);
            }

            ZSTDSeek_free(ctx);
        }

        free(buf);
    }

    return 0;
}
