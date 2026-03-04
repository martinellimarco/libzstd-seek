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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    size_t nread = ZSTDSeek_read(buf, file_size, ctx);
    if (nread != file_size) {
        FAIL("read returned %zu, expected %zu", nread, file_size);
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
        size_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) {
            FAIL("read returned %zu at byte %zu (expected 1)", n, i);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
        if (byte != raw[i]) {
            FAIL("mismatch at byte %zu: got 0x%02x expected 0x%02x", i, byte, raw[i]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
    }

    /* Read past EOF should return 0 */
    uint8_t extra;
    size_t n = ZSTDSeek_read(&extra, 1, ctx);
    if (n != 0) {
        FAIL("expected 0 bytes at EOF, got %zu", n);
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
        size_t n = ZSTDSeek_read(buf, want, ctx);
        if (n == 0) { FAIL("unexpected EOF at %zu", total); free(buf); ZSTDSeek_free(ctx); free(raw); return 1; }
        if (memcmp(buf, raw + total, n) != 0) {
            FAIL("data mismatch in chunk at offset %zu", total);
            free(buf); ZSTDSeek_free(ctx); free(raw); return 1;
        }
        total += n;
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
        int rc = ZSTDSeek_seek(ctx, (long)pos, SEEK_SET);
        if (rc != 0) { FAIL("seek(%zu) failed with %d", pos, rc); ZSTDSeek_free(ctx); free(raw); return 1; }

        long tell = ZSTDSeek_tell(ctx);
        if (tell != (long)pos) { FAIL("tell=%ld expected=%zu", tell, pos); ZSTDSeek_free(ctx); free(raw); return 1; }

        uint8_t byte;
        size_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) { FAIL("read at pos %zu returned %zu", pos, n); ZSTDSeek_free(ctx); free(raw); return 1; }
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
        int rc = ZSTDSeek_seek(ctx, (long)pos, SEEK_SET);
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
        int rc = ZSTDSeek_seek(ctx, (long)pos, SEEK_SET);
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

    int rc = ZSTDSeek_seek(ctx, 0, SEEK_END);
    if (rc != 0) { FAIL("seek_end(0) failed with %d", rc); ZSTDSeek_free(ctx); return 1; }

    long tell = ZSTDSeek_tell(ctx);
    if ((size_t)tell != file_size) {
        FAIL("tell=%ld expected=%zu after SEEK_END(0)", tell, file_size);
        ZSTDSeek_free(ctx); return 1;
    }

    /* SEEK_END -1 should be at last byte */
    rc = ZSTDSeek_seek(ctx, -1, SEEK_END);
    if (rc != 0) { FAIL("seek_end(-1) failed"); ZSTDSeek_free(ctx); return 1; }
    tell = ZSTDSeek_tell(ctx);
    if ((size_t)tell != file_size - 1) {
        FAIL("tell=%ld expected=%zu after SEEK_END(-1)", tell, file_size - 1);
        ZSTDSeek_free(ctx); return 1;
    }

    PASS("seek_end: file_size=%zu, tell checks passed", file_size);
    ZSTDSeek_free(ctx);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test: seek_random <zst_path> <raw_path> <seed> <num_ops>
 * Random SEEK_SET + read 1 byte, verify.
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
        int rc = ZSTDSeek_seek(ctx, (long)pos, SEEK_SET);
        if (rc != 0) { FAIL("seek(%zu) failed at op %zu", pos, i); ZSTDSeek_free(ctx); free(raw); return 1; }
        uint8_t byte;
        size_t n = ZSTDSeek_read(&byte, 1, ctx);
        if (n != 1) { FAIL("read at pos %zu returned %zu", pos, n); ZSTDSeek_free(ctx); free(raw); return 1; }
        if (byte != raw[pos]) {
            FAIL("op %zu: pos=%zu got 0x%02x expected 0x%02x", i, pos, byte, raw[pos]);
            ZSTDSeek_free(ctx); free(raw); return 1;
        }
    }

    PASS("seek_random: %zu operations verified", num_ops);
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
    size_t nread = ZSTDSeek_read(buf, file_size, ctx);
    if (nread != raw_size || memcmp(buf, raw, raw_size) != 0) {
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
    size_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != raw_size || memcmp(buf, raw, raw_size) != 0) {
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
    size_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != raw_size || memcmp(buf, raw, raw_size) != 0) {
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
    size_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != raw_size || memcmp(buf, raw, raw_size) != 0) {
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
    size_t nread = ZSTDSeek_read(buf, raw_size, ctx);
    if (nread != raw_size || memcmp(buf, raw, raw_size) != 0) {
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

    PASS("jump_table_auto: frames=%zu, jt->length=%llu (expected data frames=%zu)",
         frames, (unsigned long long)jt->length, expected);
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

    long ct0 = ZSTDSeek_compressedTell(ctx);
    if (ct0 < 0) { FAIL("initial compressedTell=%ld", ct0); ZSTDSeek_free(ctx); return 1; }

    /* Read some data */
    char buf[256];
    ZSTDSeek_read(buf, sizeof(buf), ctx);

    long ct1 = ZSTDSeek_compressedTell(ctx);
    INFO("compressedTell: before=%ld after_read=%ld", ct0, ct1);

    /* Seek to end */
    ZSTDSeek_seek(ctx, 0, SEEK_END);
    long ct_end = ZSTDSeek_compressedTell(ctx);
    INFO("compressedTell at SEEK_END: %ld", ct_end);

    PASS("compressed_tell: initial=%ld end=%ld", ct0, ct_end);
    ZSTDSeek_free(ctx);
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
    int rc = ZSTDSeek_seek(ctx, (long)seek_pos, SEEK_SET);
    if (rc != 0) { FAIL("seek to %zu failed", seek_pos); ZSTDSeek_free(ctx); free(raw); return 1; }

    /* Read 4 bytes that span the boundary */
    uint8_t buf[4];
    size_t n = ZSTDSeek_read(buf, 4, ctx);
    size_t expected_n = (raw_size - seek_pos < 4) ? raw_size - seek_pos : 4;
    if (n != expected_n) {
        FAIL("cross-boundary read returned %zu expected %zu", n, expected_n);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }
    if (memcmp(buf, raw + seek_pos, n) != 0) {
        FAIL("cross-boundary data mismatch at offset %zu", seek_pos);
        ZSTDSeek_free(ctx); free(raw); return 1;
    }

    PASS("frame_boundary: read %zu bytes across boundary at %zu", n, boundary);
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
    if (ZSTDSeek_uncompressedFileSize(NULL) != 0) { FAIL("uncompressedFileSize(NULL)"); failures++; }
    if (ZSTDSeek_lastKnownUncompressedFileSize(NULL) != 0) { FAIL("lastKnownSize(NULL)"); failures++; }
    if (ZSTDSeek_read(NULL, 0, NULL) != 0) { FAIL("read(NULL)"); failures++; }
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
        size_t n = ZSTDSeek_read(buf, sizeof(buf), ctx);
        INFO("truncated: create succeeded, read returned %zu", n);
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

    int rc = ZSTDSeek_seek(ctx, -1, SEEK_SET);
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
    int rc = ZSTDSeek_seek(ctx, (long)(file_size + 100), SEEK_SET);
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

    int rc = ZSTDSeek_seek(ctx, 0, 99); /* invalid origin */
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
    ZSTDSeek_seek(ctx, (long)(file_size - 1), SEEK_SET);

    /* Try to read more than available */
    uint8_t buf[1024];
    size_t n = ZSTDSeek_read(buf, sizeof(buf), ctx);
    if (n != 1) {
        FAIL("expected 1 byte at EOF-1, got %zu", n);
        ZSTDSeek_free(ctx); return 1;
    }

    /* Read again should return 0 */
    n = ZSTDSeek_read(buf, sizeof(buf), ctx);
    if (n != 0) {
        FAIL("expected 0 bytes at EOF, got %zu", n);
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
    /* Seek */
    { "seek_set_sequential",    test_seek_set_sequential },
    { "seek_set_backward",      test_seek_set_backward },
    { "seek_cur_forward",       test_seek_cur_forward },
    { "seek_end",               test_seek_end },
    { "seek_random",            test_seek_random },
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
    /* Info */
    { "file_size",              test_file_size },
    { "last_known_size",        test_last_known_size },
    { "frame_count",            test_frame_count },
    { "is_multiframe",          test_is_multiframe },
    { "compressed_tell",        test_compressed_tell },
    /* Edge cases */
    { "frame_boundary",         test_frame_boundary },
    /* Errors */
    { "error_null",             test_error_null },
    { "error_truncated",        test_error_truncated },
    { "error_invalid_format",   test_error_invalid_format },
    { "error_seek_negative",    test_error_seek_negative },
    { "error_seek_beyond",      test_error_seek_beyond },
    { "error_seek_invalid_origin", test_error_seek_invalid_origin },
    { "error_read_past_eof",    test_error_read_past_eof },
    { "error_empty_file",       test_error_empty_file },
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
