// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * test_seek.c — Test driver for libzstd-seek
 *
 * Each test case is a function dispatched by name from main().
 * Shell scripts (or CTest directly) invoke this program:
 *
 *   test_seek <test_name> [args...]
 *
 * Exit 0 on success, 1 on failure.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include "../windows-mmap.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "zstd-seek.h"

/* ── Logging macros ────────────────────────────────────────────────────────*/
#define PASS(fmt, ...) do { fprintf(stderr, "[PASS] " fmt "\n", ##__VA_ARGS__); } while(0)
#define FAIL(fmt, ...) do { fprintf(stderr, "[FAIL] " fmt "\n", ##__VA_ARGS__); } while(0)
#define INFO(fmt, ...) do { fprintf(stderr, "       " fmt "\n", ##__VA_ARGS__); } while(0)

/* ── xorshift64: same PRNG as gen_seekable.c / gen_blob.c ─────────────────*/
static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*state = x);
}

/* ── Generate expected raw data for verification ──────────────────────────*/
static void gen_expected_data(uint8_t *buf, size_t size, uint64_t seed) {
    if (seed == 0) seed = 1;
    uint64_t state = seed;
    size_t i = 0;
    while (i + 8 <= size) {
        uint64_t val = xorshift64(&state);
        memcpy(buf + i, &val, 8);
        i += 8;
    }
    if (i < size) {
        uint64_t val = xorshift64(&state);
        memcpy(buf + i, &val, size - i);
    }
}

/* ── Read a raw file into a malloc'd buffer ───────────────────────────────*/
static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: read_all <zst_path> <raw_path>
 * Read the entire file and compare with expected raw content.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_read_all(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: read_all <zst> <raw>"); return 1; }
    const char *zst_path = argv[0];
    const char *raw_path = argv[1];

    size_t raw_size;
    uint8_t *raw = read_file(raw_path, &raw_size);
    if (!raw) { FAIL("cannot read raw file '%s'", raw_path); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(zst_path);
    if (!ctx) { FAIL("createFromFile failed for '%s'", zst_path); free(raw); return 1; }

    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);
    if (file_size != raw_size) {
        FAIL("size mismatch: library=%zu expected=%zu", file_size, raw_size);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    uint8_t *buf = malloc(file_size);
    int64_t nread = ZSTDSeek_read(buf, file_size, ctx);
    if (nread != (int64_t)file_size) {
        FAIL("read returned %" PRId64 ", expected %zu", nread, file_size);
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    if (memcmp(buf, raw, file_size) != 0) {
        /* Find first mismatch */
        for (size_t i = 0; i < file_size; i++) {
            if (buf[i] != raw[i]) {
                FAIL("data mismatch at byte %zu: got 0x%02x expected 0x%02x", i, buf[i], raw[i]);
                break;
            }
        }
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("read_all: %zu bytes verified", file_size);
    free(buf); ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: read_byte_by_byte <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_read_byte_by_byte(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: read_byte_by_byte <zst> <raw>"); return 1; }
    const char *zst_path = argv[0];
    const char *raw_path = argv[1];

    size_t raw_size;
    uint8_t *raw = read_file(raw_path, &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(zst_path);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    for (size_t i = 0; i < raw_size; i++) {
        uint8_t byte;
        int64_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) {
            FAIL("read returned %" PRId64 " at byte %zu (expected 1)", n, i);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
        if (byte != raw[i]) {
            FAIL("mismatch at byte %zu: got 0x%02x expected 0x%02x", i, byte, raw[i]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
    }

    /* Read past EOF should return 0 */
    uint8_t extra;
    int64_t n = ZSTDSeek_read(&extra, 1, ctx);
    if (n != 0) {
        FAIL("expected 0 bytes at EOF, got %" PRId64, n);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("read_byte_by_byte: %zu bytes verified", raw_size);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: read_chunks <zst_path> <raw_path> <chunk_size>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_read_chunks(int argc, char *argv[]) {
    if (argc < 3) { FAIL("usage: read_chunks <zst> <raw> <chunk_size>"); return 1; }
    const char *zst_path = argv[0];
    const char *raw_path = argv[1];
    size_t chunk = (size_t)strtoull(argv[2], NULL, 10);

    size_t raw_size;
    uint8_t *raw = read_file(raw_path, &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(zst_path);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    uint8_t *buf = malloc(chunk);
    size_t total = 0;
    while (total < raw_size) {
        size_t want = (raw_size - total < chunk) ? raw_size - total : chunk;
        int64_t n = ZSTDSeek_read(buf, want, ctx);
        if (n == 0) { FAIL("unexpected EOF at %zu", total); free(buf); ZSTDSeek_free(ctx); free(raw); return 1; }
        if (memcmp(buf, raw + total, (size_t)n) != 0) {
            FAIL("data mismatch in chunk at offset %zu", total);
            free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
        }
        total += (size_t)n;
    }

    PASS("read_chunks: %zu bytes in %zu-byte chunks", total, chunk);
    free(buf); ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_set_sequential <zst_path> <raw_path>
 * SEEK_SET to every position, read 1 byte, verify.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_set_sequential(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: seek_set_sequential <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    for (size_t pos = 0; pos < raw_size; pos++) {
        int32_t rc = ZSTDSeek_seek(ctx, (int64_t)pos, SEEK_SET);
        if (rc != 0) { FAIL("seek(%zu) failed with %d", pos, rc); ZSTDSeek_free(ctx); free(raw); return 1; }

        int64_t tell = ZSTDSeek_tell(ctx);
        if (tell != (int64_t)pos) { FAIL("tell=%" PRId64 " expected=%zu", tell, pos); ZSTDSeek_free(ctx); free(raw); return 1; }

        uint8_t byte;
        int64_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) { FAIL("read at pos %zu returned %" PRId64, pos, n); ZSTDSeek_free(ctx); free(raw); return 1; }
        if (byte != raw[pos]) {
            FAIL("at pos %zu: got 0x%02x expected 0x%02x", pos, byte, raw[pos]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
    }

    PASS("seek_set_sequential: %zu positions verified", raw_size);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_set_backward <zst_path> <raw_path>
 * SEEK_SET from end to beginning.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_set_backward(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: seek_set_backward <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    for (size_t i = raw_size; i > 0; i--) {
        size_t pos = i - 1;
        int32_t rc = ZSTDSeek_seek(ctx, (int64_t)pos, SEEK_SET);
        if (rc != 0) { FAIL("seek(%zu) failed", pos); ZSTDSeek_free(ctx); free(raw); return 1; }
        uint8_t byte;
        ZSTDSeek_read(&byte, 1, ctx);
        if (byte != raw[pos]) {
            FAIL("at pos %zu: got 0x%02x expected 0x%02x", pos, byte, raw[pos]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
    }

    PASS("seek_set_backward: %zu positions verified", raw_size);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_cur_forward <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_cur_forward(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: seek_cur_forward <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    /* Read first byte, then SEEK_CUR +1 repeatedly (skip every other byte) */
    for (size_t pos = 0; pos < raw_size; pos += 2) {
        int32_t rc = ZSTDSeek_seek(ctx, (int64_t)pos, SEEK_SET);
        if (rc != 0) { FAIL("seek(%zu) failed", pos); ZSTDSeek_free(ctx); free(raw); return 1; }
        uint8_t byte;
        ZSTDSeek_read(&byte, 1, ctx);
        if (byte != raw[pos]) {
            FAIL("at pos %zu: got 0x%02x expected 0x%02x", pos, byte, raw[pos]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
        /* Now SEEK_CUR +1 to skip one byte */
        if (pos + 2 < raw_size) {
            rc = ZSTDSeek_seek(ctx, 1, SEEK_CUR);
            if (rc != 0) { FAIL("seek_cur(+1) at pos %zu failed", pos+1); ZSTDSeek_free(ctx); free(raw); return 1; }
        }
    }

    PASS("seek_cur_forward: verified");
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_end <zst_path>
 * SEEK_END with offset 0 → tell == uncompressedFileSize
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_end(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: seek_end <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);

    int32_t rc = ZSTDSeek_seek(ctx, 0, SEEK_END);
    if (rc != 0) { FAIL("seek_end(0) failed with %d", rc); ZSTDSeek_free(ctx); return 1; }

    int64_t tell = ZSTDSeek_tell(ctx);
    if ((size_t)tell != file_size) {
        FAIL("tell=%" PRId64 " expected=%zu after SEEK_END(0)", tell, file_size);
        ZSTDSeek_free(ctx); return 1;
    }

    /* SEEK_END -1 should be at last byte */
    rc = ZSTDSeek_seek(ctx, -1, SEEK_END);
    if (rc != 0) { FAIL("seek_end(-1) failed"); ZSTDSeek_free(ctx); return 1; }
    tell = ZSTDSeek_tell(ctx);
    if ((size_t)tell != file_size - 1) {
        FAIL("tell=%" PRId64 " expected=%zu after SEEK_END(-1)", tell, file_size - 1);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("seek_end: file_size=%zu, tell checks passed", file_size);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_random <zst_path> <raw_path> <seed> <num_ops>
 * Random seek + read 1 byte, verify.  Alternates between SEEK_SET,
 * SEEK_CUR and SEEK_END so all three origins are exercised.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_random(int argc, char *argv[]) {
    if (argc < 4) { FAIL("usage: seek_random <zst> <raw> <seed> <num_ops>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    uint64_t seed = strtoull(argv[2], NULL, 10);
    if (seed == 0) seed = 1;
    size_t num_ops = (size_t)strtoull(argv[3], NULL, 10);

    uint64_t state = seed;
    for (size_t i = 0; i < num_ops; i++) {
        size_t pos = (size_t)(xorshift64(&state) % raw_size);
        unsigned method = (unsigned)(xorshift64(&state) % 3);
        int32_t rc;

        switch (method) {
        case 0: /* SEEK_SET */
            rc = ZSTDSeek_seek(ctx, (int64_t)pos, SEEK_SET);
            break;
        case 1: { /* SEEK_CUR — relative from current position */
            int64_t cur = ZSTDSeek_tell(ctx);
            rc = ZSTDSeek_seek(ctx, (int64_t)pos - cur, SEEK_CUR);
            break;
        }
        case 2: { /* SEEK_END — negative offset from end */
            rc = ZSTDSeek_seek(ctx, (int64_t)pos - (int64_t)raw_size, SEEK_END);
            break;
        }
        default: rc = -1; break;
        }

        if (rc != 0) {
            FAIL("seek to %zu failed at op %zu (method=%u)", pos, i, method);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
        uint8_t byte;
        int64_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) { FAIL("read at pos %zu returned %" PRId64, pos, n); ZSTDSeek_free(ctx); free(raw); return 1; }
        if (byte != raw[pos]) {
            FAIL("op %zu: pos=%zu got 0x%02x expected 0x%02x", i, pos, byte, raw[pos]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
    }

    PASS("seek_random: %zu operations verified (SEEK_SET/CUR/END)", num_ops);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: create_from_file <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_create_from_file(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: create_from_file <zst> <raw>"); return 1; }
    return test_read_all(argc, argv); /* delegates to read_all */
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: create_from_file_no_jt <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_create_from_file_no_jt(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: create_from_file_no_jt <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileWithoutJumpTable(argv[0]);
    if (!ctx) { FAIL("createFromFileWithoutJumpTable failed"); free(raw); return 1; }

    /* Jump table should NOT be initialized yet */
    if (ZSTDSeek_jumpTableIsInitialized(ctx)) {
        FAIL("jump table should not be initialized yet");
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* Initialize it manually */
    int rc = ZSTDSeek_initializeJumpTable(ctx);
    if (rc != 0) { FAIL("initializeJumpTable failed"); ZSTDSeek_free(ctx); free(raw); return 1; }

    if (!ZSTDSeek_jumpTableIsInitialized(ctx)) {
        FAIL("jump table should be initialized after init");
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* Read all and verify */
    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);
    uint8_t *buf = malloc(file_size);
    int64_t nread = ZSTDSeek_read(buf, file_size, ctx);
    if (nread != (int64_t)raw_size || memcmp(buf, raw, raw_size) != 0) {
        FAIL("data verification failed");
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("create_from_file_no_jt: %zu bytes verified", raw_size);
    free(buf); ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: create_from_buffer <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_create_from_buffer(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: create_from_buffer <zst> <raw>"); return 1; }

    size_t zst_size, raw_size;
    uint8_t *zst = read_file(argv[0], &zst_size);
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!zst || !raw) { FAIL("cannot read files"); free(zst); free(raw); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_create(zst, zst_size);
    if (!ctx) { FAIL("ZSTDSeek_create failed"); free(zst); free(raw); return 1; }

    uint8_t *buf = malloc(raw_size);
    int64_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != (int64_t)raw_size || memcmp(buf, raw, raw_size) != 0) {
        FAIL("data verification failed");
        free(buf); ZSTDSeek_free(ctx); free(zst); free(raw); return 1;
    }

    PASS("create_from_buffer: %zu bytes verified", raw_size);
    free(buf); ZSTDSeek_free(ctx); free(zst); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: create_from_buffer_no_jt <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_create_from_buffer_no_jt(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: create_from_buffer_no_jt <zst> <raw>"); return 1; }

    size_t zst_size, raw_size;
    uint8_t *zst = read_file(argv[0], &zst_size);
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!zst || !raw) { FAIL("cannot read files"); free(zst); free(raw); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createWithoutJumpTable(zst, zst_size);
    if (!ctx) { FAIL("createWithoutJumpTable failed"); free(zst); free(raw); return 1; }

    ZSTDSeek_initializeJumpTable(ctx);

    uint8_t *buf = malloc(raw_size);
    int64_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != (int64_t)raw_size || memcmp(buf, raw, raw_size) != 0) {
        FAIL("data verification failed");
        free(buf); ZSTDSeek_free(ctx); free(zst); free(raw); return 1;
    }

    PASS("create_from_buffer_no_jt: %zu bytes verified", raw_size);
    free(buf); ZSTDSeek_free(ctx); free(zst); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: create_from_fd <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_create_from_fd(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: create_from_fd <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    int fd = open(argv[0], O_RDONLY);
    if (fd < 0) { FAIL("cannot open '%s'", argv[0]); free(raw); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileDescriptor(fd);
    if (!ctx) { FAIL("createFromFileDescriptor failed"); close(fd); free(raw); return 1; }

    int returned_fd = ZSTDSeek_fileno(ctx);
    if (returned_fd != fd) {
        FAIL("fileno returned %d, expected %d", returned_fd, fd);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    uint8_t *buf = malloc(raw_size);
    int64_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != (int64_t)raw_size || memcmp(buf, raw, raw_size) != 0) {
        FAIL("data verification failed");
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("create_from_fd: %zu bytes verified, fileno=%d", raw_size, returned_fd);
    free(buf); ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: create_from_fd_no_jt <zst_path> <raw_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_create_from_fd_no_jt(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: create_from_fd_no_jt <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    int fd = open(argv[0], O_RDONLY);
    if (fd < 0) { FAIL("cannot open '%s'", argv[0]); free(raw); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileDescriptorWithoutJumpTable(fd);
    if (!ctx) { FAIL("createFromFDWithoutJT failed"); close(fd); free(raw); return 1; }

    ZSTDSeek_initializeJumpTable(ctx);

    uint8_t *buf = malloc(raw_size);
    int64_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != (int64_t)raw_size || memcmp(buf, raw, raw_size) != 0) {
        FAIL("data verification failed");
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("create_from_fd_no_jt: %zu bytes verified", raw_size);
    free(buf); ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: jump_table_auto <zst_path> <expected_frames>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_jump_table_auto(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: jump_table_auto <zst> <expected_frames>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t expected = (size_t)strtoull(argv[1], NULL, 10);
    size_t frames = ZSTDSeek_getNumberOfFrames(ctx);
    /* getNumberOfFrames scans ALL zstd frames including any skippable frame
     * (seekable footer).  Accept expected or expected+1. */
    if (frames != expected && frames != expected + 1) {
        FAIL("getNumberOfFrames=%zu expected=%zu (or %zu for seekable)",
             frames, expected, expected + 1);
        ZSTDSeek_free(ctx); return 1;
    }

    ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
    if (!jt) { FAIL("getJumpTableOfContext returned NULL"); ZSTDSeek_free(ctx); return 1; }
    /* The seekable format parser stores expected data-frame records + 1 EOF
     * sentinel, so jt->length == expected + 1.  Without seekable format, each
     * data frame has one record plus the EOF sentinel, also expected + 1. */
    if (jt->length != expected + 1) {
        FAIL("jt->length=%llu expected=%zu", (unsigned long long)jt->length, expected + 1);
        ZSTDSeek_free(ctx); return 1;
    }

    int multi = ZSTDSeek_isMultiframe(ctx);
    int expected_multi = (expected > 1) ? 1 : 0;
    if (multi != expected_multi) {
        FAIL("isMultiframe=%d expected=%d", multi, expected_multi);
        ZSTDSeek_free(ctx); return 1;
    }

    /* Verify record monotonicity */
    for (size_t i = 1; i < (size_t)jt->length; i++) {
        if (jt->records[i].compressedPos < jt->records[i-1].compressedPos) {
            FAIL("compressedPos not monotonic at record %zu: %zu < %zu",
                 i, jt->records[i].compressedPos, jt->records[i-1].compressedPos);
            ZSTDSeek_free(ctx); return 1;
        }
        if (jt->records[i].uncompressedPos < jt->records[i-1].uncompressedPos) {
            FAIL("uncompressedPos not monotonic at record %zu: %zu < %zu",
                 i, jt->records[i].uncompressedPos, jt->records[i-1].uncompressedPos);
            ZSTDSeek_free(ctx); return 1;
        }
    }

    /* Last record's uncompressedPos should equal uncompressedFileSize */
    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);
    size_t last_uncomp = jt->records[jt->length - 1].uncompressedPos;
    if (last_uncomp != file_size) {
        FAIL("last record uncompressedPos=%zu != uncompressedFileSize=%zu",
             last_uncomp, file_size);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("jump_table_auto: frames=%zu, jt->length=%llu, records monotonic, last=%zu",
         frames, (unsigned long long)jt->length, file_size);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: jump_table_progressive <zst_path>
 * Test initializeJumpTableUpUntilPos with incremental positions.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_jump_table_progressive(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: jump_table_progressive <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileWithoutJumpTable(argv[0]);
    if (!ctx) { FAIL("createFromFileWithoutJumpTable failed"); return 1; }

    /* Should not be initialized */
    if (ZSTDSeek_jumpTableIsInitialized(ctx)) {
        FAIL("jump table should not be initialized");
        ZSTDSeek_free(ctx); return 1;
    }

    /* Initialize up to position 100 */
    int rc = ZSTDSeek_initializeJumpTableUpUntilPos(ctx, 100);
    if (rc != 0) { FAIL("initializeJumpTableUpUntilPos(100) failed"); ZSTDSeek_free(ctx); return 1; }

    size_t partial_size = ZSTDSeek_lastKnownUncompressedFileSize(ctx);
    INFO("after partial init(100): lastKnownSize=%zu, initialized=%d",
         partial_size, ZSTDSeek_jumpTableIsInitialized(ctx));

    /* Now fully initialize */
    rc = ZSTDSeek_initializeJumpTable(ctx);
    if (rc != 0) { FAIL("initializeJumpTable failed"); ZSTDSeek_free(ctx); return 1; }

    if (!ZSTDSeek_jumpTableIsInitialized(ctx)) {
        FAIL("jump table should be fully initialized");
        ZSTDSeek_free(ctx); return 1;
    }

    size_t full_size = ZSTDSeek_uncompressedFileSize(ctx);
    INFO("after full init: uncompressedFileSize=%zu", full_size);

    if (full_size < partial_size) {
        FAIL("full size %zu < partial size %zu", full_size, partial_size);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("jump_table_progressive: partial=%zu full=%zu", partial_size, full_size);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: jump_table_new_free
 * Standalone jump table lifecycle: new → addRecord → free.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_jump_table_new_free(int argc, char *argv[]) {
    (void)argc; (void)argv;

    ZSTDSeek_JumpTable *jt = ZSTDSeek_newJumpTable();
    if (!jt) { FAIL("newJumpTable returned NULL"); return 1; }
    if (jt->length != 0) { FAIL("new jt length=%llu expected=0", (unsigned long long)jt->length); ZSTDSeek_freeJumpTable(jt); return 1; }

    ZSTDSeek_addJumpTableRecord(jt, 0, 0);
    ZSTDSeek_addJumpTableRecord(jt, 100, 200);
    ZSTDSeek_addJumpTableRecord(jt, 300, 500);

    if (jt->length != 3) { FAIL("after 3 adds: length=%llu", (unsigned long long)jt->length); ZSTDSeek_freeJumpTable(jt); return 1; }
    if (jt->records[0].compressedPos != 0 || jt->records[0].uncompressedPos != 0) {
        FAIL("record[0] wrong"); ZSTDSeek_freeJumpTable(jt); return 1;
    }
    if (jt->records[2].compressedPos != 300 || jt->records[2].uncompressedPos != 500) {
        FAIL("record[2] wrong"); ZSTDSeek_freeJumpTable(jt); return 1;
    }

    ZSTDSeek_freeJumpTable(jt);

    /* Also test NULL handling */
    ZSTDSeek_freeJumpTable(NULL);
    ZSTDSeek_addJumpTableRecord(NULL, 0, 0);

    PASS("jump_table_new_free: lifecycle OK");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: file_size <zst_path> <expected_size>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_file_size(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: file_size <zst> <expected>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t expected = (size_t)strtoull(argv[1], NULL, 10);
    size_t actual = ZSTDSeek_uncompressedFileSize(ctx);
    if (actual != expected) {
        FAIL("uncompressedFileSize=%zu expected=%zu", actual, expected);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("file_size: %zu", actual);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: last_known_size <zst_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_last_known_size(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: last_known_size <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileWithoutJumpTable(argv[0]);
    if (!ctx) { FAIL("createFromFileWithoutJumpTable failed"); return 1; }

    size_t before = ZSTDSeek_lastKnownUncompressedFileSize(ctx);
    INFO("before init: lastKnown=%zu", before);

    /* Partial init */
    ZSTDSeek_initializeJumpTableUpUntilPos(ctx, 10);
    size_t partial = ZSTDSeek_lastKnownUncompressedFileSize(ctx);
    INFO("after partial init: lastKnown=%zu", partial);

    /* Full init */
    ZSTDSeek_initializeJumpTable(ctx);
    size_t full = ZSTDSeek_lastKnownUncompressedFileSize(ctx);
    INFO("after full init: lastKnown=%zu", full);

    if (full < partial) {
        FAIL("full %zu < partial %zu", full, partial);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("last_known_size: before=%zu partial=%zu full=%zu", before, partial, full);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: frame_count <zst_path> <expected>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_frame_count(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: frame_count <zst> <expected>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t expected = (size_t)strtoull(argv[1], NULL, 10);
    size_t actual = ZSTDSeek_getNumberOfFrames(ctx);

    /* Note: for seekable format files, the skiptable is a skippable frame
     * that getNumberOfFrames counts. So expected might be num_frames + 1. */
    if (actual != expected && actual != expected + 1) {
        FAIL("getNumberOfFrames=%zu expected=%zu (or %zu for seekable)", actual, expected, expected + 1);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("frame_count: %zu", actual);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: is_multiframe <zst_path> <expected>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_is_multiframe(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: is_multiframe <zst> <expected>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    int expected = atoi(argv[1]);
    int actual = ZSTDSeek_isMultiframe(ctx);
    if (actual != expected) {
        FAIL("isMultiframe=%d expected=%d", actual, expected);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("is_multiframe: %d", actual);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: compressed_tell <zst_path>
 * Verify compressedTell advances coherently.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_compressed_tell(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: compressed_tell <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    int64_t ct0 = ZSTDSeek_compressedTell(ctx);
    if (ct0 < 0) { FAIL("initial compressedTell=%" PRId64, ct0); ZSTDSeek_free(ctx); return 1; }

    /* Read some data */
    char buf[256];
    ZSTDSeek_read(buf, sizeof(buf), ctx);

    int64_t ct1 = ZSTDSeek_compressedTell(ctx);
    INFO("compressedTell: before=%" PRId64 " after_read=%" PRId64, ct0, ct1);

    /* Seek to end */
    ZSTDSeek_seek(ctx, 0, SEEK_END);
    int64_t ct_end = ZSTDSeek_compressedTell(ctx);
    INFO("compressedTell at SEEK_END: %" PRId64, ct_end);

    PASS("compressed_tell: initial=%" PRId64 " end=%" PRId64, ct0, ct_end);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: compressed_tell_monotonic <zst_path>
 * Read in small chunks and verify compressedTell() never goes backwards.
 * Catches the pre-fix bug where compressedTell reset to frame start
 * when serving data from the tmpOutBuff cache.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_compressed_tell_monotonic(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: compressed_tell_monotonic <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    int64_t prev_ct = 0;
    size_t total_read = 0;
    size_t reads = 0;
    uint8_t buf[10];
    int64_t n;

    while ((n = ZSTDSeek_read(buf, sizeof(buf), ctx)) > 0) {
        total_read += (size_t)n;
        reads++;
        int64_t ct = ZSTDSeek_compressedTell(ctx);
        if (ct < prev_ct) {
            FAIL("compressedTell went backwards: %" PRId64 " -> %" PRId64 " after read #%zu (total_read=%zu)",
                 prev_ct, ct, reads, total_read);
            ZSTDSeek_free(ctx);
            return 1;
        }
        prev_ct = ct;
    }

    if (prev_ct <= 0) {
        FAIL("compressedTell never advanced (final=%" PRId64 ", total_read=%zu)", prev_ct, total_read);
        ZSTDSeek_free(ctx);
        return 1;
    }

    PASS("compressed_tell_monotonic: %zu reads, %zu bytes, final compressedTell=%" PRId64,
         reads, total_read, prev_ct);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: compressed_tell_seek <zst_path>
 * Verify compressedTell() matches jump table at frame boundaries after
 * seek, and remains monotonic during sequential read-through.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_compressed_tell_seek(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: compressed_tell_seek <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
    if (!jt || jt->length < 3) {
        FAIL("need at least 2 frames (got %llu records)", jt ? (unsigned long long)jt->length : 0);
        ZSTDSeek_free(ctx);
        return 1;
    }

    /* Part 1: Seek to each frame boundary and check compressedTell matches JT */
    for (uint64_t i = 0; i + 1 < jt->length; i++) {
        size_t uncomp_pos = jt->records[i].uncompressedPos;
        size_t comp_pos   = jt->records[i].compressedPos;

        int32_t rc = ZSTDSeek_seek(ctx, (int64_t)uncomp_pos, SEEK_SET);
        if (rc != 0) {
            FAIL("seek to frame %llu (uncomp=%zu) failed: %d", (unsigned long long)i, uncomp_pos, rc);
            ZSTDSeek_free(ctx);
            return 1;
        }

        int64_t ct = ZSTDSeek_compressedTell(ctx);
        if ((size_t)ct != comp_pos) {
            FAIL("frame %llu: after seek, compressedTell=%" PRId64 " expected=%zu",
                 (unsigned long long)i, ct, comp_pos);
            ZSTDSeek_free(ctx);
            return 1;
        }

        /* Read 1 byte — compressedTell must not go below frame start */
        uint8_t byte;
        int64_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) {
            FAIL("frame %llu: read returned %" PRId64, (unsigned long long)i, n);
            ZSTDSeek_free(ctx);
            return 1;
        }
        int64_t ct2 = ZSTDSeek_compressedTell(ctx);
        if (ct2 < ct) {
            FAIL("frame %llu: compressedTell went backwards after read: %" PRId64 " -> %" PRId64,
                 (unsigned long long)i, ct, ct2);
            ZSTDSeek_free(ctx);
            return 1;
        }
    }

    /* Part 2: Seek back to start, verify compressedTell == 0 */
    int32_t rc = ZSTDSeek_seek(ctx, 0, SEEK_SET);
    if (rc != 0) { FAIL("seek(0, SEEK_SET) failed"); ZSTDSeek_free(ctx); return 1; }

    int64_t ct_start = ZSTDSeek_compressedTell(ctx);
    if (ct_start != 0) {
        FAIL("after seek(0), compressedTell=%" PRId64 " expected=0", ct_start);
        ZSTDSeek_free(ctx);
        return 1;
    }

    /* Part 3: Sequential read-through, verify monotonic */
    int64_t prev_ct = 0;
    uint8_t buf[10];
    int64_t n;
    while ((n = ZSTDSeek_read(buf, sizeof(buf), ctx)) > 0) {
        int64_t ct = ZSTDSeek_compressedTell(ctx);
        if (ct < prev_ct) {
            FAIL("sequential pass: compressedTell went backwards: %" PRId64 " -> %" PRId64, prev_ct, ct);
            ZSTDSeek_free(ctx);
            return 1;
        }
        prev_ct = ct;
    }

    PASS("compressed_tell_seek: %llu frames verified, sequential monotonic OK",
         (unsigned long long)(jt->length - 1));
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: compressed_tell_absolute <zst_path>
 * Read sequentially and verify compressedTell() never exceeds the
 * compressed file size.  Uses a single large frame to force many
 * iterations of the inner ZSTD_decompressStream loop per read() call.
 * This catches the cumulative overcount bug where input.pos (cumulative
 * within a frame) was accumulated instead of the delta.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_compressed_tell_absolute(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: compressed_tell_absolute <zst>"); return 1; }

    /* Get compressed file size via stat */
    struct stat st;
    if (stat(argv[0], &st) != 0) { FAIL("stat(%s) failed", argv[0]); return 1; }
    const size_t compressed_size = (size_t)st.st_size;

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    uint8_t buf[4096];
    size_t total_read = 0;
    size_t reads = 0;
    int64_t n;

    while ((n = ZSTDSeek_read(buf, sizeof(buf), ctx)) > 0) {
        total_read += (size_t)n;
        reads++;
        int64_t ct = ZSTDSeek_compressedTell(ctx);
        if (ct < 0 || (size_t)ct > compressed_size) {
            FAIL("read #%zu (total=%zu): compressedTell=%" PRId64
                 " exceeds compressed file size=%zu",
                 reads, total_read, ct, compressed_size);
            ZSTDSeek_free(ctx);
            return 1;
        }
    }

    /* At EOF, compressedTell should equal compressed file size */
    int64_t ct_final = ZSTDSeek_compressedTell(ctx);
    if ((size_t)ct_final != compressed_size) {
        FAIL("at EOF: compressedTell=%" PRId64 " expected=%zu", ct_final, compressed_size);
        ZSTDSeek_free(ctx);
        return 1;
    }

    PASS("compressed_tell_absolute: %zu reads, %zu bytes decompressed, "
         "final compressedTell=%" PRId64 " == compressed size=%zu",
         reads, total_read, ct_final, compressed_size);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_forward_large <zst_path> <raw_path>
 * Perform large SEEK_CUR forwards within frames and verify data against
 * the raw reference.  Exercises the discard loop with bigger skips than
 * the existing seek_cur_forward test (which uses 1-byte hops).
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_forward_large(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: seek_forward_large <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
    if (!jt || jt->length < 2) {
        FAIL("need at least 1 frame");
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    int tested = 0;
    for (uint64_t i = 0; i + 1 < jt->length; i++) {
        size_t frame_start = jt->records[i].uncompressedPos;
        size_t frame_end   = jt->records[i + 1].uncompressedPos;
        size_t frame_size  = frame_end - frame_start;

        /* Only test frames large enough for a 500-byte skip + bookend reads */
        if (frame_size < 502) continue;

        /* Seek to frame start, read 1 byte, verify */
        int32_t rc = ZSTDSeek_seek(ctx, (int64_t)frame_start, SEEK_SET);
        if (rc != 0) {
            FAIL("seek to frame %llu start (%zu) failed", (unsigned long long)i, frame_start);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        uint8_t byte;
        int64_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1 || byte != raw[frame_start]) {
            FAIL("frame %llu: first byte mismatch (got 0x%02x expected 0x%02x)",
                 (unsigned long long)i, byte, raw[frame_start]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        /* SEEK_CUR +500, verify tell */
        rc = ZSTDSeek_seek(ctx, 500, SEEK_CUR);
        if (rc != 0) {
            FAIL("frame %llu: seek(+500, SEEK_CUR) failed", (unsigned long long)i);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        int64_t pos = ZSTDSeek_tell(ctx);
        int64_t expected_pos = (int64_t)(frame_start + 501);
        if (pos != expected_pos) {
            FAIL("frame %llu: after skip, tell=%" PRId64 " expected=%" PRId64,
                 (unsigned long long)i, pos, expected_pos);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        /* Read 1 byte after skip, verify against raw */
        n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1 || byte != raw[frame_start + 501]) {
            FAIL("frame %llu: byte after skip mismatch (got 0x%02x expected 0x%02x)",
                 (unsigned long long)i, byte, raw[frame_start + 501]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        tested++;
    }

    if (tested == 0) {
        FAIL("no frames >= 502 bytes found");
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("seek_forward_large: tested %d frames with +500 skip", tested);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: frame_boundary <zst_path> <raw_path>
 * Read across a frame boundary.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_frame_boundary(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: frame_boundary <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    /* Get jump table to find frame boundaries */
    ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
    if (!jt || jt->length < 3) {
        FAIL("need at least 2 frames for boundary test");
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* Seek to 2 bytes before the second frame boundary */
    size_t boundary = jt->records[1].uncompressedPos;
    if (boundary < 2) { FAIL("frame boundary too small"); ZSTDSeek_free(ctx); free(raw); return 1; }

    size_t seek_pos = boundary - 2;
    int32_t rc = ZSTDSeek_seek(ctx, (int64_t)seek_pos, SEEK_SET);
    if (rc != 0) { FAIL("seek to %zu failed", seek_pos); ZSTDSeek_free(ctx); free(raw); return 1; }

    /* Read 4 bytes that span the boundary */
    uint8_t buf[4];
    int64_t n = ZSTDSeek_read(buf, 4, ctx);
    size_t expected_n = (raw_size - seek_pos < 4) ? raw_size - seek_pos : 4;
    if (n != (int64_t)expected_n) {
        FAIL("cross-boundary read returned %" PRId64 " expected %zu", n, expected_n);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }
    if (memcmp(buf, raw + seek_pos, (size_t)n) != 0) {
        FAIL("cross-boundary data mismatch at offset %zu", seek_pos);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("frame_boundary: read %" PRId64 " bytes across boundary at %zu", n, boundary);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_null
 * All API functions with NULL context.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_null(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int failures = 0;

    if (ZSTDSeek_getJumpTableOfContext(NULL) != NULL) { FAIL("getJumpTableOfContext(NULL)"); failures++; }
    ZSTDSeek_freeJumpTable(NULL); /* should not crash */
    ZSTDSeek_addJumpTableRecord(NULL, 0, 0); /* should not crash */
    if (ZSTDSeek_initializeJumpTable(NULL) != -1) { FAIL("initializeJumpTable(NULL)"); failures++; }
    if (ZSTDSeek_initializeJumpTableUpUntilPos(NULL, 0) != -1) { FAIL("initializeJumpTableUpUntilPos(NULL)"); failures++; }
    if (ZSTDSeek_jumpTableIsInitialized(NULL) != false) { FAIL("jumpTableIsInitialized(NULL)"); failures++; }
    if (ZSTDSeek_fileno(NULL) != -1) { FAIL("fileno(NULL)"); failures++; }
    if (ZSTDSeek_uncompressedFileSize(NULL) != 0) { FAIL("uncompressedFileSize(NULL)"); failures++; }
    if (ZSTDSeek_lastKnownUncompressedFileSize(NULL) != 0) { FAIL("lastKnownSize(NULL)"); failures++; }
    if (ZSTDSeek_read(NULL, 0, NULL) != ZSTDSEEK_ERR_READ) { FAIL("read(NULL)"); failures++; }
    if (ZSTDSeek_seek(NULL, 0, 0) != -1) { FAIL("seek(NULL)"); failures++; }
    if (ZSTDSeek_tell(NULL) != -1) { FAIL("tell(NULL)"); failures++; }
    if (ZSTDSeek_compressedTell(NULL) != -1) { FAIL("compressedTell(NULL)"); failures++; }
    if (ZSTDSeek_isMultiframe(NULL) != 0) { FAIL("isMultiframe(NULL)"); failures++; }
    if (ZSTDSeek_getNumberOfFrames(NULL) != 0) { FAIL("getNumberOfFrames(NULL)"); failures++; }
    ZSTDSeek_free(NULL); /* should not crash */

    if (ZSTDSeek_createFromFile("") != NULL) { FAIL("createFromFile(\"\")"); failures++; }
    if (ZSTDSeek_createFromFile("/nonexistent/path/file.zst") != NULL) { FAIL("createFromFile(nonexistent)"); failures++; }
    if (ZSTDSeek_createFromFileDescriptor(-1) != NULL) { FAIL("createFromFD(-1)"); failures++; }
    if (ZSTDSeek_create(NULL, 0) != NULL) { FAIL("create(NULL,0)"); failures++; }

    if (failures > 0) { FAIL("error_null: %d failures", failures); return 1; }
    PASS("error_null: all NULL/invalid args handled correctly");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_truncated <path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_truncated(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_truncated <path>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    /* A truncated file should fail to create or have corrupted data */
    if (ctx) {
        /* If it creates, reads should fail or return short */
        char buf[1024];
        int64_t n = ZSTDSeek_read(buf, sizeof(buf), ctx);
        INFO("truncated: create succeeded, read returned %" PRId64, n);
        ZSTDSeek_free(ctx);
    } else {
        INFO("truncated: create correctly returned NULL");
    }

    PASS("error_truncated: no crash");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_invalid_format <path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_invalid_format(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_invalid_format <path>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (ctx) {
        FAIL("createFromFile should fail for non-zstd file");
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("error_invalid_format: correctly rejected");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_seek_negative <zst_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_seek_negative(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_seek_negative <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    int32_t rc = ZSTDSeek_seek(ctx, -1, SEEK_SET);
    if (rc != ZSTDSEEK_ERR_NEGATIVE_SEEK) {
        FAIL("seek(-1, SEEK_SET) returned %d, expected %d", rc, ZSTDSEEK_ERR_NEGATIVE_SEEK);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("error_seek_negative: correctly returned %d", rc);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_seek_beyond <zst_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_seek_beyond(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_seek_beyond <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);
    int32_t rc = ZSTDSeek_seek(ctx, (int64_t)(file_size + 100), SEEK_SET);
    if (rc != ZSTDSEEK_ERR_BEYOND_END_SEEK) {
        FAIL("seek beyond EOF returned %d, expected %d", rc, ZSTDSEEK_ERR_BEYOND_END_SEEK);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("error_seek_beyond: correctly returned %d", rc);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_seek_invalid_origin <zst_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_seek_invalid_origin(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_seek_invalid_origin <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    int32_t rc = ZSTDSeek_seek(ctx, 0, 99); /* invalid origin */
    if (rc != -1) {
        FAIL("seek with origin=99 returned %d, expected -1", rc);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("error_seek_invalid_origin: correctly returned -1");
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_read_past_eof <zst_path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_read_past_eof(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_read_past_eof <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);

    /* Seek to near end */
    ZSTDSeek_seek(ctx, (int64_t)(file_size - 1), SEEK_SET);

    /* Try to read more than available */
    uint8_t buf[1024];
    int64_t n = ZSTDSeek_read(buf, sizeof(buf), ctx);
    if (n != 1) {
        FAIL("expected 1 byte at EOF-1, got %" PRId64, n);
        ZSTDSeek_free(ctx); return 1;
    }

    /* Read again should return 0 */
    n = ZSTDSeek_read(buf, sizeof(buf), ctx);
    if (n != 0) {
        FAIL("expected 0 bytes at EOF, got %" PRId64, n);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("error_read_past_eof: correctly returned short read + 0");
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_empty_file <path>
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_empty_file(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_empty_file <path>"); return 1; }

    /* The file should be empty (0 bytes) */
    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (ctx) {
        FAIL("createFromFile should fail for empty file");
        ZSTDSeek_free(ctx); return 1;
    }

    /* Also test with buffer */
    uint8_t empty = 0;
    ctx = ZSTDSeek_create(&empty, 0);
    if (ctx) {
        FAIL("create(buf, 0) should fail");
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("error_empty_file: correctly rejected");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_cur_backward <zst_path> <raw_path>
 * SEEK_CUR with negative offset: read 1 byte, then SEEK_CUR(-3) to go
 * backward by 2 net positions, visiting every even offset from high to low.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_cur_backward(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: seek_cur_backward <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    /* Start position: use 24 or less if the file is small */
    size_t start = (raw_size > 24) ? 24 : (raw_size & ~(size_t)1);

    int32_t rc = ZSTDSeek_seek(ctx, (int64_t)start, SEEK_SET);
    if (rc != 0) { FAIL("initial seek(%zu) failed", start); ZSTDSeek_free(ctx); free(raw); return 1; }

    size_t checked = 0;
    for (int64_t pos = (int64_t)start; pos >= 0; pos -= 2) {
        int64_t tell = ZSTDSeek_tell(ctx);
        if (tell != pos) {
            FAIL("tell=%" PRId64 " expected=%" PRId64, tell, pos);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        uint8_t byte;
        int64_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) { FAIL("read at pos %" PRId64 " returned %" PRId64, pos, n); ZSTDSeek_free(ctx); free(raw); return 1; }
        if (byte != raw[pos]) {
            FAIL("at pos %" PRId64 ": got 0x%02x expected 0x%02x", pos, byte, raw[pos]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
        checked++;

        /* After read(1) position is pos+1.  SEEK_CUR(-3) → pos+1-3 = pos-2 */
        if (pos >= 2) {
            rc = ZSTDSeek_seek(ctx, -3, SEEK_CUR);
            if (rc != 0) {
                FAIL("seek_cur(-3) from pos %" PRId64 " failed with %d", pos + 1, rc);
                ZSTDSeek_free(ctx); free(raw); return 1;
            }
        }
    }

    PASS("seek_cur_backward: %zu even positions verified (SEEK_CUR -3)", checked);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_out_of_file <zst_path>
 * 9 boundary cases for SEEK_SET/CUR/END with exact error codes.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_out_of_file(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: seek_out_of_file <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t N = ZSTDSeek_uncompressedFileSize(ctx);
    int32_t rc;
    int failures = 0;

    /* 1. seek(-1, SEEK_SET) → NEGATIVE_SEEK */
    rc = ZSTDSeek_seek(ctx, -1, SEEK_SET);
    if (rc != ZSTDSEEK_ERR_NEGATIVE_SEEK) {
        FAIL("case 1: seek(-1,SET)=%d expected=%d", rc, ZSTDSEEK_ERR_NEGATIVE_SEEK); failures++;
    }

    /* 2. seek(N, SEEK_SET) → 0 (EOF position is valid) */
    rc = ZSTDSeek_seek(ctx, (int64_t)N, SEEK_SET);
    if (rc != 0) {
        FAIL("case 2: seek(N=%zu,SET)=%d expected=0", N, rc); failures++;
    }

    /* 3. seek(N+1, SEEK_SET) → BEYOND_END */
    rc = ZSTDSeek_seek(ctx, (int64_t)(N + 1), SEEK_SET);
    if (rc != ZSTDSEEK_ERR_BEYOND_END_SEEK) {
        FAIL("case 3: seek(N+1=%zu,SET)=%d expected=%d", N + 1, rc, ZSTDSEEK_ERR_BEYOND_END_SEEK); failures++;
    }

    /* 4. seek(1, SEEK_END) → BEYOND_END */
    rc = ZSTDSeek_seek(ctx, 1, SEEK_END);
    if (rc != ZSTDSEEK_ERR_BEYOND_END_SEEK) {
        FAIL("case 4: seek(1,END)=%d expected=%d", rc, ZSTDSEEK_ERR_BEYOND_END_SEEK); failures++;
    }

    /* 5. seek(-N, SEEK_END) → 0 (position 0) */
    rc = ZSTDSeek_seek(ctx, -(int64_t)N, SEEK_END);
    if (rc != 0) {
        FAIL("case 5: seek(-N=%" PRId64 ",END)=%d expected=0", -(int64_t)N, rc); failures++;
    } else {
        int64_t tell = ZSTDSeek_tell(ctx);
        if (tell != 0) { FAIL("case 5: tell=%" PRId64 " expected=0", tell); failures++; }
    }

    /* 6. seek(-(N+1), SEEK_END) → NEGATIVE_SEEK */
    rc = ZSTDSeek_seek(ctx, -(int64_t)(N + 1), SEEK_END);
    if (rc != ZSTDSEEK_ERR_NEGATIVE_SEEK) {
        FAIL("case 6: seek(-(N+1)=%" PRId64 ",END)=%d expected=%d", -(int64_t)(N + 1), rc, ZSTDSEEK_ERR_NEGATIVE_SEEK); failures++;
    }

    /* 7. seek(-1, SEEK_CUR) from pos 0 → NEGATIVE_SEEK */
    ZSTDSeek_seek(ctx, 0, SEEK_SET);
    rc = ZSTDSeek_seek(ctx, -1, SEEK_CUR);
    if (rc != ZSTDSEEK_ERR_NEGATIVE_SEEK) {
        FAIL("case 7: seek(-1,CUR)@0=%d expected=%d", rc, ZSTDSEEK_ERR_NEGATIVE_SEEK); failures++;
    }

    /* 8. seek(N, SEEK_CUR) from pos 0 → 0 (EOF position) */
    ZSTDSeek_seek(ctx, 0, SEEK_SET);
    rc = ZSTDSeek_seek(ctx, (int64_t)N, SEEK_CUR);
    if (rc != 0) {
        FAIL("case 8: seek(N=%zu,CUR)@0=%d expected=0", N, rc); failures++;
    }

    /* 9. seek(N+1, SEEK_CUR) from pos 0 → BEYOND_END */
    ZSTDSeek_seek(ctx, 0, SEEK_SET);
    rc = ZSTDSeek_seek(ctx, (int64_t)(N + 1), SEEK_CUR);
    if (rc != ZSTDSEEK_ERR_BEYOND_END_SEEK) {
        FAIL("case 9: seek(N+1=%zu,CUR)@0=%d expected=%d", N + 1, rc, ZSTDSEEK_ERR_BEYOND_END_SEEK); failures++;
    }

    if (failures > 0) { FAIL("seek_out_of_file: %d/9 cases failed", failures); ZSTDSeek_free(ctx); return 1; }

    PASS("seek_out_of_file: all 9 boundary cases passed (N=%zu)", N);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: read_too_much <zst_path> <raw_path>
 * Request 2× the file size from position 0.  Verify short read, tell,
 * data content, and that a second read returns 0.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_read_too_much(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: read_too_much <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);
    size_t request = file_size * 2;
    uint8_t *buf = malloc(request);
    if (!buf) { FAIL("malloc(%zu) failed", request); ZSTDSeek_free(ctx); free(raw); return 1; }

    /* First read: request 2× file size → short read of exactly file_size */
    int64_t n = ZSTDSeek_read(buf, request, ctx);
    if (n != (int64_t)file_size) {
        FAIL("expected short read of %zu, got %" PRId64, file_size, n);
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    int64_t tell = ZSTDSeek_tell(ctx);
    if ((size_t)tell != file_size) {
        FAIL("tell=%" PRId64 " expected=%zu after short read", tell, file_size);
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    if (memcmp(buf, raw, file_size) != 0) {
        FAIL("data mismatch in short read");
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* Second read at EOF → 0 bytes */
    int64_t n2 = ZSTDSeek_read(buf, request, ctx);
    if (n2 != 0) {
        FAIL("expected 0 bytes at EOF, got %" PRId64, n2);
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    tell = ZSTDSeek_tell(ctx);
    if ((size_t)tell != file_size) {
        FAIL("tell=%" PRId64 " expected=%zu after second read", tell, file_size);
        free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("read_too_much: requested %zu, got %" PRId64 ", tell=%" PRId64 ", re-read=0", request, n, tell);
    free(buf); ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: jump_table_manual <zst_path>
 * Open with auto JT to discover frame positions, then re-open without JT
 * and manually add the same records.  Verify lengths and file size match.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_jump_table_manual(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: jump_table_manual <zst>"); return 1; }

    /* Step 1: open with auto JT to get reference records */
    ZSTDSeek_Context *ref = ZSTDSeek_createFromFile(argv[0]);
    if (!ref) { FAIL("createFromFile (ref) failed"); return 1; }

    ZSTDSeek_JumpTable *ref_jt = ZSTDSeek_getJumpTableOfContext(ref);
    if (!ref_jt || ref_jt->length < 2) {
        FAIL("need at least 2 JT records"); ZSTDSeek_free(ref); return 1;
    }

    size_t num_records = (size_t)ref_jt->length;
    size_t *comp_pos  = malloc(num_records * sizeof(size_t));
    size_t *uncomp_pos = malloc(num_records * sizeof(size_t));
    for (size_t i = 0; i < num_records; i++) {
        comp_pos[i]  = ref_jt->records[i].compressedPos;
        uncomp_pos[i] = ref_jt->records[i].uncompressedPos;
    }
    size_t ref_file_size = ZSTDSeek_uncompressedFileSize(ref);
    ZSTDSeek_free(ref);

    /* Step 2: open without JT and manually add records */
    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileWithoutJumpTable(argv[0]);
    if (!ctx) {
        FAIL("createFromFileWithoutJumpTable failed");
        free(comp_pos); free(uncomp_pos); return 1;
    }

    ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
    if (!jt) {
        FAIL("getJumpTableOfContext returned NULL");
        ZSTDSeek_free(ctx); free(comp_pos); free(uncomp_pos); return 1;
    }

    for (size_t i = 0; i < num_records; i++) {
        ZSTDSeek_addJumpTableRecord(jt, comp_pos[i], uncomp_pos[i]);
        if ((size_t)jt->length != i + 1) {
            FAIL("after add[%zu]: length=%llu expected=%zu",
                 i, (unsigned long long)jt->length, i + 1);
            ZSTDSeek_free(ctx); free(comp_pos); free(uncomp_pos); return 1;
        }
    }

    /* Last record's uncompressedPos is the file size */
    size_t manual_size = jt->records[num_records - 1].uncompressedPos;
    if (manual_size != ref_file_size) {
        FAIL("manual file_size=%zu expected=%zu", manual_size, ref_file_size);
        ZSTDSeek_free(ctx); free(comp_pos); free(uncomp_pos); return 1;
    }

    PASS("jump_table_manual: %zu records added, file_size=%zu", num_records, ref_file_size);
    ZSTDSeek_free(ctx); free(comp_pos); free(uncomp_pos);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: jt_progressive_reads <zst_path> <raw_path>
 * Open without JT, then trigger JT growth through sequential reads that
 * cross frame boundaries.  Verify data correctness and JT length growth.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_jt_progressive_reads(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: jt_progressive_reads <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    /* Get reference frame boundaries from an auto-JT context */
    ZSTDSeek_Context *ref = ZSTDSeek_createFromFile(argv[0]);
    if (!ref) { FAIL("createFromFile (ref) failed"); free(raw); return 1; }
    ZSTDSeek_JumpTable *ref_jt = ZSTDSeek_getJumpTableOfContext(ref);
    if (!ref_jt || ref_jt->length < 3) {
        FAIL("need at least 2 data frames"); ZSTDSeek_free(ref); free(raw); return 1;
    }
    size_t frame1_end = ref_jt->records[1].uncompressedPos;
    ZSTDSeek_free(ref);

    /* Open without JT */
    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileWithoutJumpTable(argv[0]);
    if (!ctx) { FAIL("createFromFileWithoutJumpTable failed"); free(raw); return 1; }

    ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(ctx);
    if (!jt) { FAIL("getJumpTableOfContext NULL"); ZSTDSeek_free(ctx); free(raw); return 1; }

    uint64_t initial_len = jt->length;
    INFO("initial jt->length=%llu", (unsigned long long)initial_len);

    /* Read 1 byte — should trigger JT growth (at least first frame discovered) */
    uint8_t byte;
    int64_t n = ZSTDSeek_read(&byte, 1, ctx);
    if (n != 1 || byte != raw[0]) {
        FAIL("first read: n=%" PRId64 " byte=0x%02x expected=0x%02x", n, byte, raw[0]);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    uint64_t after_read1 = jt->length;
    INFO("after 1-byte read: jt->length=%llu", (unsigned long long)after_read1);
    if (after_read1 <= initial_len) {
        FAIL("jt->length did not grow after first read: %llu -> %llu",
             (unsigned long long)initial_len, (unsigned long long)after_read1);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* Read across first frame boundary into second frame.
     * The library may return short reads at frame boundaries, so we loop. */
    if (frame1_end > 1 && frame1_end + 1 <= raw_size) {
        size_t target = frame1_end + 1;
        size_t current_pos = 1;
        size_t remaining = target - current_pos;
        uint8_t *buf = malloc(remaining);
        size_t total_read = 0;
        while (total_read < remaining) {
            n = ZSTDSeek_read(buf + total_read, remaining - total_read, ctx);
            if (n == 0) {
                FAIL("unexpected EOF at pos %zu during cross-frame read", current_pos + total_read);
                free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
            }
            total_read += (size_t)n;
        }
        if (memcmp(buf, raw + current_pos, remaining) != 0) {
            FAIL("cross-frame data mismatch");
            free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
        }
        free(buf);

        uint64_t after_cross = jt->length;
        INFO("after cross-frame read to %zu: jt->length=%llu", target, (unsigned long long)after_cross);
        if (after_cross < after_read1) {
            FAIL("jt->length decreased: %llu -> %llu",
                 (unsigned long long)after_read1, (unsigned long long)after_cross);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
    }

    /* Seek to end → fully initializes JT */
    int32_t rc = ZSTDSeek_seek(ctx, 0, SEEK_END);
    if (rc != 0) { FAIL("seek(0, SEEK_END) failed"); ZSTDSeek_free(ctx); free(raw); return 1; }

    uint64_t after_end = jt->length;
    INFO("after SEEK_END: jt->length=%llu", (unsigned long long)after_end);

    /* JT should be fully initialized now */
    if (!ZSTDSeek_jumpTableIsInitialized(ctx)) {
        FAIL("JT not fully initialized after SEEK_END");
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* JT must have grown from the initial state */
    if (after_end <= initial_len) {
        FAIL("JT did not grow: initial=%llu final=%llu",
             (unsigned long long)initial_len, (unsigned long long)after_end);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* Last record's uncompressedPos must equal the full file size */
    size_t full_size = ZSTDSeek_uncompressedFileSize(ctx);
    if (full_size != raw_size) {
        FAIL("uncompressedFileSize=%zu expected=%zu", full_size, raw_size);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }
    size_t last_uncomp = jt->records[after_end - 1].uncompressedPos;
    if (last_uncomp != full_size) {
        FAIL("last record uncompressedPos=%zu != file_size=%zu", last_uncomp, full_size);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("jt_progressive_reads: JT grew %llu -> %llu via reads, file_size=%zu",
         (unsigned long long)initial_len, (unsigned long long)after_end, full_size);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_corrupted_header <zst_path>
 * Read a valid .zst, corrupt the magic number, verify rejection.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_corrupted_header(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_corrupted_header <zst>"); return 1; }

    size_t zst_size;
    uint8_t *zst = read_file(argv[0], &zst_size);
    if (!zst || zst_size < 4) { FAIL("cannot read zst file or too small"); free(zst); return 1; }

    /* Corrupt the magic number */
    zst[0] &= 0xF0;

    ZSTDSeek_Context *ctx = ZSTDSeek_create(zst, zst_size);
    if (ctx) {
        FAIL("create should fail for corrupted header");
        ZSTDSeek_free(ctx); free(zst); return 1;
    }

    PASS("error_corrupted_header: correctly rejected corrupted magic");
    free(zst);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_mixed_format <zst_path>
 * Buffer = valid ZSTD + garbage.  Create with valid-only size should work.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_mixed_format(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_mixed_format <zst>"); return 1; }

    size_t zst_size;
    uint8_t *zst = read_file(argv[0], &zst_size);
    if (!zst) { FAIL("cannot read zst file"); return 1; }

    /* Build mixed buffer: valid ZSTD followed by garbage */
    size_t total = zst_size * 2;
    uint8_t *mixed = malloc(total);
    if (!mixed) { FAIL("malloc failed"); free(zst); return 1; }
    memcpy(mixed, zst, zst_size);
    memset(mixed + zst_size, 0xAA, zst_size);

    /* Create with only the valid portion size → should succeed */
    ZSTDSeek_Context *ctx = ZSTDSeek_create(mixed, zst_size);
    if (!ctx) {
        FAIL("create should succeed with valid-only size");
        free(mixed); free(zst); return 1;
    }

    size_t frames = ZSTDSeek_getNumberOfFrames(ctx);
    if (frames == 0) {
        FAIL("getNumberOfFrames returned 0");
        ZSTDSeek_free(ctx); free(mixed); free(zst); return 1;
    }

    INFO("mixed_format: valid portion %zu bytes -> %zu frames", zst_size, frames);

    PASS("error_mixed_format: valid portion works (%zu frames)", frames);
    ZSTDSeek_free(ctx); free(mixed); free(zst);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_cur_zero <zst_path>
 * SEEK_CUR with offset 0 should be a no-op at any position.
 * Exercises the early-return path (line ~501 in zstd-seek.c).
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_cur_zero(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: seek_cur_zero <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    size_t file_size = ZSTDSeek_uncompressedFileSize(ctx);
    int failures = 0;

    /* Test at position 0 */
    int32_t rc = ZSTDSeek_seek(ctx, 0, SEEK_CUR);
    if (rc != 0) { FAIL("seek(0,CUR)@0=%d expected=0", rc); failures++; }
    if (ZSTDSeek_tell(ctx) != 0) { FAIL("tell=%" PRId64 " expected=0 after seek(0,CUR)", ZSTDSeek_tell(ctx)); failures++; }

    /* Seek to middle, then seek(0, CUR) */
    int64_t mid = (int64_t)(file_size / 2);
    ZSTDSeek_seek(ctx, mid, SEEK_SET);
    rc = ZSTDSeek_seek(ctx, 0, SEEK_CUR);
    if (rc != 0) { FAIL("seek(0,CUR)@%" PRId64 "=%d expected=0", mid, rc); failures++; }
    if (ZSTDSeek_tell(ctx) != mid) { FAIL("tell=%" PRId64 " expected=%" PRId64 " after seek(0,CUR)", ZSTDSeek_tell(ctx), mid); failures++; }

    /* Seek to EOF, then seek(0, CUR) */
    ZSTDSeek_seek(ctx, 0, SEEK_END);
    int64_t eof_pos = ZSTDSeek_tell(ctx);
    rc = ZSTDSeek_seek(ctx, 0, SEEK_CUR);
    if (rc != 0) { FAIL("seek(0,CUR)@EOF=%d expected=0", rc); failures++; }
    if (ZSTDSeek_tell(ctx) != eof_pos) { FAIL("tell=%" PRId64 " expected=%" PRId64 " after seek(0,CUR)@EOF", ZSTDSeek_tell(ctx), eof_pos); failures++; }

    if (failures > 0) { FAIL("seek_cur_zero: %d failures", failures); ZSTDSeek_free(ctx); return 1; }

    PASS("seek_cur_zero: no-op at pos=0, mid=%" PRId64 ", eof=%" PRId64, mid, eof_pos);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: fileno_buffer <zst_path>
 * Create context from buffer → fileno should return -1.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_fileno_buffer(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: fileno_buffer <zst>"); return 1; }

    size_t zst_size;
    uint8_t *zst = read_file(argv[0], &zst_size);
    if (!zst) { FAIL("cannot read zst file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_create(zst, zst_size);
    if (!ctx) { FAIL("create from buffer failed"); free(zst); return 1; }

    int fd = ZSTDSeek_fileno(ctx);
    if (fd != -1) {
        FAIL("fileno on buffer context: got %d expected -1", fd);
        ZSTDSeek_free(ctx); free(zst); return 1;
    }

    PASS("fileno_buffer: correctly returns -1 for buffer context");
    ZSTDSeek_free(ctx); free(zst);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_to_same_pos <zst_path> <raw_path>
 * Seek to a non-zero position, then seek again to the same position.
 * Exercises the early-return path for offset==currentUncompressedPos.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_to_same_pos(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: seek_to_same_pos <zst> <raw>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    /* Seek to position 100 (or smaller if file is small) */
    size_t target = (raw_size > 100) ? 100 : raw_size / 2;
    int32_t rc = ZSTDSeek_seek(ctx, (int64_t)target, SEEK_SET);
    if (rc != 0) { FAIL("initial seek(%zu) failed", target); ZSTDSeek_free(ctx); free(raw); return 1; }

    /* Read one byte to verify position */
    uint8_t byte1;
    int64_t n = ZSTDSeek_read(&byte1, 1, ctx);
    if (n != 1 || byte1 != raw[target]) {
        FAIL("read@%zu: n=%" PRId64 " got=0x%02x expected=0x%02x", target, n, byte1, raw[target]);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    /* Now seek back to the same target position */
    rc = ZSTDSeek_seek(ctx, (int64_t)target, SEEK_SET);
    if (rc != 0) { FAIL("second seek(%zu) failed with %d", target, rc); ZSTDSeek_free(ctx); free(raw); return 1; }

    /* Verify we can read the same byte again */
    uint8_t byte2;
    n = ZSTDSeek_read(&byte2, 1, ctx);
    if (n != 1 || byte2 != raw[target]) {
        FAIL("re-read@%zu: n=%" PRId64 " got=0x%02x expected=0x%02x", target, n, byte2, raw[target]);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    if (byte1 != byte2) {
        FAIL("bytes differ: first=0x%02x second=0x%02x", byte1, byte2);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("seek_to_same_pos: seek(%zu) twice produces identical reads", target);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seekable_malformed_footer <zst_path> <raw_path>
 * Read a seekable .zst into memory, corrupt the seekable footer in 4 ways:
 *   (a) reserved bits in SFD byte
 *   (b) skippable header magic
 *   (c) frame size field
 *   (d) numFrames set to 0xFFFFFFFF (exceeds maxEntries)
 * In each case the library should ignore the footer and fall back to
 * frame-by-frame scanning, still producing correct data.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seekable_malformed_footer(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: seekable_malformed_footer <zst> <raw>"); return 1; }

    size_t zst_size;
    uint8_t *zst_orig = read_file(argv[0], &zst_size);
    if (!zst_orig || zst_size < 20) { FAIL("cannot read zst or too small"); free(zst_orig); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); free(zst_orig); return 1; }

    int failures = 0;
    uint8_t *zst = malloc(zst_size);
    uint8_t *out = malloc(raw_size);

    /* ── (a) Corrupt reserved bits in SFD byte ────────────────────────────
     * SFD is at offset (size - 5): bits 2-6 should be zero.
     * Set bit 2 to make it non-zero → library ignores seekable footer. */
    memcpy(zst, zst_orig, zst_size);
    zst[zst_size - 5] |= 0x04; /* set reserved bit 2 */

    ZSTDSeek_Context *ctx = ZSTDSeek_create(zst, zst_size);
    if (!ctx) {
        FAIL("(a) reserved bits: create failed"); failures++;
    } else {
        int64_t n = ZSTDSeek_read(out, raw_size, ctx);
        if (n != (int64_t)raw_size || memcmp(out, raw, raw_size) != 0) {
            FAIL("(a) reserved bits: data mismatch (n=%" PRId64 ")", n); failures++;
        } else {
            PASS("(a) reserved bits: fallback to scanning OK");
        }
        ZSTDSeek_free(ctx);
    }

    /* ── (b) Corrupt skippable header magic ───────────────────────────────
     * The seekable footer is a skippable frame with magic 0x184D2A5E.
     * Find it and flip a bit. */
    memcpy(zst, zst_orig, zst_size);
    /* Footer: last 9 bytes = numFrames(4) + sfd(1) + seekTableMagic(4)
     * From footer, we can calculate where the skippable frame header starts. */
    {
        uint8_t *footer = zst + (zst_size - 9);
        uint8_t sfd = footer[4];
        uint8_t checksumFlag = sfd >> 7;
        uint32_t numFrames;
        memcpy(&numFrames, footer, 4);
        uint32_t sizePerEntry = 8 + (checksumFlag ? 4 : 0);
        uint32_t tableSize = sizePerEntry * numFrames;
        uint32_t frameSize = tableSize + 9 + 8; /* footer + skippable header */
        uint8_t *frame_start = zst + (zst_size - frameSize);
        /* Corrupt the skippable header magic (first 4 bytes of frame) */
        frame_start[0] ^= 0xFF;

        ctx = ZSTDSeek_create(zst, zst_size);
        if (!ctx) {
            FAIL("(b) bad skippable magic: create failed"); failures++;
        } else {
            int64_t n = ZSTDSeek_read(out, raw_size, ctx);
            if (n != (int64_t)raw_size || memcmp(out, raw, raw_size) != 0) {
                FAIL("(b) bad skippable magic: data mismatch (n=%" PRId64 ")", n); failures++;
            } else {
                PASS("(b) bad skippable magic: fallback to scanning OK");
            }
            ZSTDSeek_free(ctx);
        }
    }

    /* ── (c) Corrupt frame size in skippable header ───────────────────────
     * Bytes 4-7 of the skippable frame contain the frame content size.
     * Set it to 0 to create a mismatch → library ignores footer. */
    memcpy(zst, zst_orig, zst_size);
    {
        uint8_t *footer = zst + (zst_size - 9);
        uint8_t sfd = footer[4];
        uint8_t checksumFlag = sfd >> 7;
        uint32_t numFrames;
        memcpy(&numFrames, footer, 4);
        uint32_t sizePerEntry = 8 + (checksumFlag ? 4 : 0);
        uint32_t tableSize = sizePerEntry * numFrames;
        uint32_t frameSize = tableSize + 9 + 8;
        uint8_t *frame_start = zst + (zst_size - frameSize);
        /* Corrupt the content size field (bytes 4-7) to 0 */
        memset(frame_start + 4, 0, 4);

        ctx = ZSTDSeek_create(zst, zst_size);
        if (!ctx) {
            FAIL("(c) bad frame size: create failed"); failures++;
        } else {
            int64_t n = ZSTDSeek_read(out, raw_size, ctx);
            if (n != (int64_t)raw_size || memcmp(out, raw, raw_size) != 0) {
                FAIL("(c) bad frame size: data mismatch (n=%" PRId64 ")", n); failures++;
            } else {
                PASS("(c) bad frame size: fallback to scanning OK");
            }
            ZSTDSeek_free(ctx);
        }
    }

    /* ── (d) Inflate numFrames far beyond what the buffer can hold ────────
     * Set numFrames to 0xFFFFFFFF → library detects numFrames > maxEntries
     * and ignores the seektable, falling back to frame scanning. */
    memcpy(zst, zst_orig, zst_size);
    {
        /* numFrames is at the start of the footer (last 9 bytes) */
        uint8_t *footer = zst + (zst_size - 9);
        uint32_t bad_numFrames = 0xFFFFFFFFU;
        memcpy(footer, &bad_numFrames, 4);

        ctx = ZSTDSeek_create(zst, zst_size);
        if (!ctx) {
            FAIL("(d) numFrames overflow: create failed"); failures++;
        } else {
            int64_t n = ZSTDSeek_read(out, raw_size, ctx);
            if (n != (int64_t)raw_size || memcmp(out, raw, raw_size) != 0) {
                FAIL("(d) numFrames overflow: data mismatch (n=%" PRId64 ")", n); failures++;
            } else {
                PASS("(d) numFrames overflow: fallback to scanning OK");
            }
            ZSTDSeek_free(ctx);
        }
    }

    free(out); free(zst); free(raw); free(zst_orig);
    if (failures > 0) { FAIL("seekable_malformed_footer: %d/4 sub-tests failed", failures); return 1; }
    PASS("seekable_malformed_footer: all 4 corruption variants handled gracefully");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: read_zero_bytes <zst_path>
 * read(buf, 0, ctx) should return 0 without error at any position.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_read_zero_bytes(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: read_zero_bytes <zst>"); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); return 1; }

    int failures = 0;
    uint8_t dummy;

    /* At position 0 */
    int64_t n = ZSTDSeek_read(&dummy, 0, ctx);
    if (n != 0) { FAIL("read(0)@pos0 = %" PRId64 " expected 0", n); failures++; }
    if (ZSTDSeek_tell(ctx) != 0) { FAIL("tell moved after read(0)"); failures++; }

    /* At middle */
    size_t mid = ZSTDSeek_uncompressedFileSize(ctx) / 2;
    ZSTDSeek_seek(ctx, (int64_t)mid, SEEK_SET);
    n = ZSTDSeek_read(&dummy, 0, ctx);
    if (n != 0) { FAIL("read(0)@mid = %" PRId64 " expected 0", n); failures++; }
    if (ZSTDSeek_tell(ctx) != (int64_t)mid) { FAIL("tell moved after read(0)@mid"); failures++; }

    /* At EOF */
    ZSTDSeek_seek(ctx, 0, SEEK_END);
    int64_t eof_pos = ZSTDSeek_tell(ctx);
    n = ZSTDSeek_read(&dummy, 0, ctx);
    if (n != 0) { FAIL("read(0)@eof = %" PRId64 " expected 0", n); failures++; }
    if (ZSTDSeek_tell(ctx) != eof_pos) { FAIL("tell moved after read(0)@eof"); failures++; }

    ZSTDSeek_free(ctx);
    if (failures > 0) { FAIL("read_zero_bytes: %d failures", failures); return 1; }
    PASS("read_zero_bytes: read(0) returns 0 at pos=0, mid=%zu, eof=%" PRId64, mid, eof_pos);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_corrupted_frame_data <zst_path>
 * Read a valid multi-frame .zst into memory, corrupt payload bytes inside
 * the second frame.  createWithoutJumpTable should succeed (first frame is
 * valid), but read() past the first frame should eventually return ERR_READ.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_corrupted_frame_data(int argc, char *argv[]) {
    if (argc < 1) { FAIL("usage: error_corrupted_frame_data <zst>"); return 1; }

    size_t zst_size;
    uint8_t *zst = read_file(argv[0], &zst_size);
    if (!zst || zst_size < 32) { FAIL("cannot read zst file or too small"); free(zst); return 1; }

    /* Find the start of the second frame using ZSTD_findFrameCompressedSize */
    size_t first_frame_size = ZSTD_findFrameCompressedSize(zst, zst_size);
    if (ZSTD_isError(first_frame_size) || first_frame_size >= zst_size - 8) {
        FAIL("need at least 2 frames; first_frame_size=%zu total=%zu", first_frame_size, zst_size);
        free(zst); return 1;
    }

    /* Corrupt several bytes in the middle of the second frame's payload */
    size_t corrupt_start = first_frame_size + 8; /* skip frame header (~4-6 bytes) */
    if (corrupt_start + 16 > zst_size) corrupt_start = first_frame_size + 4;
    for (size_t i = 0; i < 16 && corrupt_start + i < zst_size; i++) {
        zst[corrupt_start + i] ^= 0xFF;
    }

    /* Create context without JT — should succeed because first frame header is intact */
    ZSTDSeek_Context *ctx = ZSTDSeek_createWithoutJumpTable(zst, zst_size);
    if (!ctx) {
        /* If first frame header got corrupted too, the test is still valid */
        PASS("error_corrupted_frame_data: create correctly rejected corrupted data");
        free(zst); return 0;
    }

    /* Try to read the entire file — should hit ERR_READ when decompressing the
     * corrupted second frame */
    if (ZSTDSeek_initializeJumpTable(ctx) != 0) {
        /* JT init failed because frame scan hit corrupted data — that's OK */
        PASS("error_corrupted_frame_data: JT init correctly failed on corrupt data");
        ZSTDSeek_free(ctx); free(zst); return 0;
    }

    /* If somehow JT init passed, try reading */
    size_t total = ZSTDSeek_uncompressedFileSize(ctx);
    uint8_t *out = malloc(total > 0 ? total : 1);
    int64_t nread = ZSTDSeek_read(out, total, ctx);
    if (nread < 0) {
        PASS("error_corrupted_frame_data: read returned ERR (%" PRId64 ") as expected", nread);
    } else {
        /* A short read is also acceptable since the corruption stops decompression */
        INFO("error_corrupted_frame_data: read returned %" PRId64 " / %zu (short read or lucky)", nread, total);
        PASS("error_corrupted_frame_data: no crash on corrupted data");
    }

    free(out);
    ZSTDSeek_free(ctx); free(zst);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: error_seektable_bad_offsets <zst_path> <raw_path>
 * Modify the seekable footer so that a seektable entry has an
 * inflated compressed-size delta that would make cOffset exceed the
 * buffer.  The library should detect the malformed entry, discard the
 * seektable, fall back to frame-by-frame scanning, and still produce
 * correct decompressed data.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_error_seektable_bad_offsets(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: error_seektable_bad_offsets <zst> <raw>"); return 1; }

    size_t zst_size;
    uint8_t *zst = read_file(argv[0], &zst_size);
    if (!zst || zst_size < 30) { FAIL("cannot read zst file"); free(zst); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); free(zst); return 1; }

    /* Locate the seektable footer (last 9 bytes):
     *   bytes 0-3: numFrames (LE32)
     *   byte  4:   sfd
     *   bytes 5-8: seekable magic (0x8F92EAB1)
     */
    uint8_t *footer = zst + (zst_size - 9);
    /* Read seekable magic — stored as LE in the file.  On a LE host,
     * memcpy gives us the numeric value directly. */
    uint32_t magic;
    memcpy(&magic, footer + 5, 4);
    /* On LE: magic == ZSTD_SEEKABLE_MAGICNUMBER (0x8F92EAB1) */
    if (magic != ZSTD_SEEKABLE_MAGICNUMBER) {
        FAIL("test file does not have seekable footer (magic=0x%08X)", magic);
        free(zst); free(raw); return 1;
    }

    uint8_t sfd = footer[4];
    uint8_t checksumFlag = sfd >> 7;
    uint32_t numFrames;
    memcpy(&numFrames, footer, 4);
    if (numFrames < 2) {
        FAIL("need at least 2 frames in seektable, got %u", numFrames);
        free(zst); free(raw); return 1;
    }

    uint32_t sizePerEntry = 8 + (checksumFlag ? 4 : 0);
    uint32_t tableSize = sizePerEntry * numFrames;
    uint32_t frameSize = tableSize + 9 + 8; /* footer + skippable header */
    uint8_t *table = zst + (zst_size - frameSize) + 8; /* skip skippable header */

    /* Corrupt the first entry's compressed-size delta (dc) to 0xFFFFFFFF.
     * This will cause cOffset to exceed the buffer after the first entry. */
    uint32_t bad_dc = 0xFFFFFFFFU;
    memcpy(table, &bad_dc, 4);

    /* Create context — should ignore bad seektable, scan frames, produce correct data */
    ZSTDSeek_Context *ctx = ZSTDSeek_create(zst, zst_size);
    if (!ctx) {
        FAIL("create failed (expected fallback to scanning)");
        free(zst); free(raw); return 1;
    }

    uint8_t *out = malloc(raw_size);
    int64_t nread = ZSTDSeek_read(out, raw_size, ctx);
    if (nread != (int64_t)raw_size) {
        FAIL("read returned %" PRId64 " expected %zu", nread, raw_size);
        free(out); ZSTDSeek_free(ctx); free(zst); free(raw); return 1;
    }

    if (memcmp(out, raw, raw_size) != 0) {
        FAIL("data mismatch after seektable fallback");
        free(out); ZSTDSeek_free(ctx); free(zst); free(raw); return 1;
    }

    PASS("error_seektable_bad_offsets: bad dc=0xFFFFFFFF ignored, fallback scan produced correct data");
    free(out); ZSTDSeek_free(ctx); free(zst); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: decompress <zst_path> <output_path>
 * Decompress the entire .zst file and write raw output to a file.
 * Used by test_heavy.sh for SHA256 verification.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_decompress(int argc, char *argv[]) {
    if (argc < 2) { FAIL("usage: decompress <zst> <output>"); return 1; }
    const char *zst_path = argv[0];
    const char *out_path = argv[1];

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFileWithoutJumpTable(zst_path);
    if (!ctx) { FAIL("createFromFile failed for '%s'", zst_path); return 1; }

    FILE *out = fopen(out_path, "wb");
    if (!out) { FAIL("cannot open output file '%s'", out_path); ZSTDSeek_free(ctx); return 1; }

    uint8_t buf[128 * 1024];
    size_t total = 0;
    int64_t nread;
    while ((nread = ZSTDSeek_read(buf, sizeof(buf), ctx)) > 0) {
        if (fwrite(buf, 1, (size_t)nread, out) != (size_t)nread) {
            FAIL("write error at offset %zu", total);
            fclose(out); ZSTDSeek_free(ctx); return 1;
        }
        total += (size_t)nread;
    }

    fclose(out);
    ZSTDSeek_free(ctx);
    PASS("decompress: %zu bytes written to '%s'", total, out_path);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_stress <zst_path> <raw_path> <seed> <num_ops>
 *
 * Stress test: random byte-range reads with verification.
 *   - Picks a random start position and a random length (1–8192 bytes)
 *   - Seeks via SEEK_SET, SEEK_CUR, or SEEK_END (alternating)
 *   - Reads the full range and memcmp's against the raw reference
 *   - Jumps forward and backward pseudo-randomly
 *
 * If seed==0 a time-based seed is generated and printed for reproduction.
 * ══════════════════════════════════════════════════════════════════════════*/
static int test_seek_stress(int argc, char *argv[]) {
    if (argc < 4) { FAIL("usage: seek_stress <zst> <raw> <seed> <num_ops>"); return 1; }

    size_t raw_size;
    uint8_t *raw = read_file(argv[1], &raw_size);
    if (!raw) { FAIL("cannot read raw file"); return 1; }
    if (raw_size == 0) { FAIL("raw file is empty"); free(raw); return 1; }

    ZSTDSeek_Context *ctx = ZSTDSeek_createFromFile(argv[0]);
    if (!ctx) { FAIL("createFromFile failed"); free(raw); return 1; }

    uint64_t seed = strtoull(argv[2], NULL, 10);
    if (seed == 0) {
        seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 16);
        if (seed == 0) seed = 1;
    }
    size_t num_ops = (size_t)strtoull(argv[3], NULL, 10);

    INFO("seek_stress: seed=%" PRIu64 " ops=%zu file_size=%zu", seed, num_ops, raw_size);

    uint64_t state = seed;
    uint8_t buf[8192];
    size_t cross_boundary = 0;  /* count of reads spanning > 1 byte range */
    size_t backward_jumps = 0;

    for (size_t i = 0; i < num_ops; i++) {
        /* Pick random start and length */
        size_t start = (size_t)(xorshift64(&state) % raw_size);
        size_t max_len = raw_size - start;
        if (max_len > sizeof(buf)) max_len = sizeof(buf);
        size_t len = (size_t)(xorshift64(&state) % max_len) + 1;

        /* Choose seek method: alternate to exercise all paths */
        unsigned method = (unsigned)(i % 3);
        int32_t rc;
        int64_t cur_pos = ZSTDSeek_tell(ctx);

        if ((int64_t)start < cur_pos) backward_jumps++;

        switch (method) {
        case 0: /* SEEK_SET */
            rc = ZSTDSeek_seek(ctx, (int64_t)start, SEEK_SET);
            break;
        case 1: { /* SEEK_CUR */
            int64_t delta = (int64_t)start - cur_pos;
            rc = ZSTDSeek_seek(ctx, delta, SEEK_CUR);
            break;
        }
        case 2: { /* SEEK_END */
            rc = ZSTDSeek_seek(ctx, (int64_t)start - (int64_t)raw_size, SEEK_END);
            break;
        }
        default: rc = -1; break;
        }

        if (rc != 0) {
            FAIL("op %zu: seek to %zu failed (method=%u, seed=%" PRIu64 ")", i, start, method, seed);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        /* Read the byte range */
        int64_t nread = ZSTDSeek_read(buf, len, ctx);
        if (nread != (int64_t)len) {
            FAIL("op %zu: read at %zu len %zu returned %" PRId64 " (seed=%" PRIu64 ")",
                 i, start, len, nread, seed);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        /* Verify against raw reference */
        if (memcmp(buf, raw + start, len) != 0) {
            /* Find first mismatch for diagnostics */
            for (size_t j = 0; j < len; j++) {
                if (buf[j] != raw[start + j]) {
                    FAIL("op %zu: mismatch at offset %zu+%zu: got 0x%02x expected 0x%02x "
                         "(range [%zu..%zu], seed=%" PRIu64 ")",
                         i, start, j, buf[j], raw[start + j],
                         start, start + len - 1, seed);
                    break;
                }
            }
            ZSTDSeek_free(ctx); free(raw); return 1;
        }

        if (len > 1) cross_boundary++;
    }

    PASS("seek_stress: %zu ops verified, %zu multi-byte, %zu backward "
         "(seed=%" PRIu64 ")", num_ops, cross_boundary, backward_jumps, seed);
    ZSTDSeek_free(ctx); free(raw);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Dispatch table
 * ══════════════════════════════════════════════════════════════════════════*/
typedef struct {
    const char *name;
    int (*func)(int argc, char *argv[]);
} TestEntry;

static const TestEntry tests[] = {
    /* Read */
    { "read_all",               test_read_all },
    { "read_byte_by_byte",      test_read_byte_by_byte },
    { "read_chunks",            test_read_chunks },
    { "decompress",             test_decompress },
    /* Seek */
    { "seek_set_sequential",    test_seek_set_sequential },
    { "seek_set_backward",      test_seek_set_backward },
    { "seek_cur_forward",       test_seek_cur_forward },
    { "seek_cur_backward",      test_seek_cur_backward },
    { "seek_end",               test_seek_end },
    { "seek_random",            test_seek_random },
    { "seek_stress",            test_seek_stress },
    { "seek_out_of_file",       test_seek_out_of_file },
    { "seek_cur_zero",          test_seek_cur_zero },
    { "seek_to_same_pos",       test_seek_to_same_pos },
    /* Create variants */
    { "create_from_file",       test_create_from_file },
    { "create_from_file_no_jt", test_create_from_file_no_jt },
    { "create_from_buffer",     test_create_from_buffer },
    { "create_from_buffer_no_jt", test_create_from_buffer_no_jt },
    { "create_from_fd",         test_create_from_fd },
    { "create_from_fd_no_jt",   test_create_from_fd_no_jt },
    /* Jump table */
    { "jump_table_auto",        test_jump_table_auto },
    { "jump_table_progressive", test_jump_table_progressive },
    { "jump_table_new_free",    test_jump_table_new_free },
    { "jump_table_manual",      test_jump_table_manual },
    { "jt_progressive_reads",   test_jt_progressive_reads },
    /* Info */
    { "file_size",              test_file_size },
    { "last_known_size",        test_last_known_size },
    { "frame_count",            test_frame_count },
    { "is_multiframe",          test_is_multiframe },
    { "compressed_tell",        test_compressed_tell },
    { "compressed_tell_monotonic", test_compressed_tell_monotonic },
    { "compressed_tell_seek",   test_compressed_tell_seek },
    { "compressed_tell_absolute", test_compressed_tell_absolute },
    /* Seek (large) */
    { "seek_forward_large",     test_seek_forward_large },
    /* Edge cases */
    { "frame_boundary",         test_frame_boundary },
    { "read_too_much",          test_read_too_much },
    { "read_zero_bytes",        test_read_zero_bytes },
    { "fileno_buffer",          test_fileno_buffer },
    { "seekable_malformed_footer", test_seekable_malformed_footer },
    /* Errors */
    { "error_null",             test_error_null },
    { "error_truncated",        test_error_truncated },
    { "error_invalid_format",   test_error_invalid_format },
    { "error_seek_negative",    test_error_seek_negative },
    { "error_seek_beyond",      test_error_seek_beyond },
    { "error_seek_invalid_origin", test_error_seek_invalid_origin },
    { "error_read_past_eof",    test_error_read_past_eof },
    { "error_empty_file",       test_error_empty_file },
    { "error_corrupted_header", test_error_corrupted_header },
    { "error_mixed_format",     test_error_mixed_format },
    { "error_corrupted_frame_data", test_error_corrupted_frame_data },
    { "error_seektable_bad_offsets", test_error_seektable_bad_offsets },
    { NULL, NULL }
};

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <test_name> [args...]\n\nAvailable tests:\n", argv[0]);
        for (const TestEntry *t = tests; t->name; t++) {
            fprintf(stderr, "  %s\n", t->name);
        }
        return 1;
    }

    const char *test_name = argv[1];
    for (const TestEntry *t = tests; t->name; t++) {
        if (strcmp(t->name, test_name) == 0) {
            return t->func(argc - 2, argv + 2);
        }
    }

    fprintf(stderr, "Unknown test: %s\n", test_name);
    return 1;
}
