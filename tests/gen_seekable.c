// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * gen_seekable.c — Deterministic multi-frame zstd file generator
 *
 * Generates a zstd file with N frames of deterministic random content.
 * Optionally appends a seekable format footer (skiptable).
 *
 * Usage: gen_seekable <seed> <num_frames> <frame_size> <output.zst> [flags...]
 *
 * Flags:
 *   --seekable          append seekable format footer
 *   --no-content-size   do not pledge content size in frame headers
 *   --checksum          add checksum to seekable format entries
 *   --level N           compression level (default: 1)
 *   --vary-size         frame_i has size = frame_size * (i+1) / num_frames
 *   --dump-raw FILE     also write uncompressed data to FILE
 *
 * Given the same seed and parameters, always produces identical output.
 * Uses xorshift64 (same PRNG as t2sz gen_blob.c).
 *
 * Exit 0 on success, 1 on error.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

/* ── xorshift64: period 2^64-1, passes BigCrush ────────────────────────────*/
static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*state = x);
}

/* ── Generate deterministic raw data for one frame ─────────────────────────*/
static void gen_frame_data(uint8_t *buf, size_t size, uint64_t seed) {
    if (seed == 0) seed = 1; /* xorshift64 must not start at zero */
    uint64_t state = seed;
    size_t i = 0;
    /* Write full 8-byte words */
    while (i + 8 <= size) {
        uint64_t val = xorshift64(&state);
        memcpy(buf + i, &val, 8);
        i += 8;
    }
    /* Write remaining bytes */
    if (i < size) {
        uint64_t val = xorshift64(&state);
        memcpy(buf + i, &val, size - i);
    }
}

/* ── Write a little-endian uint32_t ────────────────────────────────────────*/
static void write_le32(FILE *f, uint32_t val) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(val);
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val >> 16);
    buf[3] = (uint8_t)(val >> 24);
    fwrite(buf, 1, 4, f);
}

/* ── XXH32 for seekable format checksum (simplified) ──────────────────────
 * The seekable format spec uses XXH32 of the decompressed data.
 * We use a minimal implementation here.
 */
#define XXH_PRIME32_1 0x9E3779B1U
#define XXH_PRIME32_2 0x85EBCA77U
#define XXH_PRIME32_3 0xC2B2AE3DU
#define XXH_PRIME32_4 0x27D4EB2FU
#define XXH_PRIME32_5 0x165667B1U

static uint32_t xxh32_rotl(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static uint32_t xxh32(const void *input, size_t len) {
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *end = p + len;
    uint32_t h32;

    if (len >= 16) {
        const uint8_t *limit = end - 16;
        uint32_t v1 = 0 + XXH_PRIME32_1 + XXH_PRIME32_2;
        uint32_t v2 = 0 + XXH_PRIME32_2;
        uint32_t v3 = 0;
        uint32_t v4 = 0 - XXH_PRIME32_1;
        do {
            uint32_t k;
            memcpy(&k, p, 4); v1 += k * XXH_PRIME32_2; v1 = xxh32_rotl(v1, 13); v1 *= XXH_PRIME32_1; p += 4;
            memcpy(&k, p, 4); v2 += k * XXH_PRIME32_2; v2 = xxh32_rotl(v2, 13); v2 *= XXH_PRIME32_1; p += 4;
            memcpy(&k, p, 4); v3 += k * XXH_PRIME32_2; v3 = xxh32_rotl(v3, 13); v3 *= XXH_PRIME32_1; p += 4;
            memcpy(&k, p, 4); v4 += k * XXH_PRIME32_2; v4 = xxh32_rotl(v4, 13); v4 *= XXH_PRIME32_1; p += 4;
        } while (p <= limit);
        h32 = xxh32_rotl(v1, 1) + xxh32_rotl(v2, 7) + xxh32_rotl(v3, 12) + xxh32_rotl(v4, 18);
    } else {
        h32 = 0 + XXH_PRIME32_5;
    }
    h32 += (uint32_t)len;

    while (p + 4 <= end) {
        uint32_t k;
        memcpy(&k, p, 4);
        h32 += k * XXH_PRIME32_3;
        h32 = xxh32_rotl(h32, 17) * XXH_PRIME32_4;
        p += 4;
    }
    while (p < end) {
        h32 += (*p) * XXH_PRIME32_5;
        h32 = xxh32_rotl(h32, 11) * XXH_PRIME32_1;
        p++;
    }

    h32 ^= h32 >> 15;
    h32 *= XXH_PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= XXH_PRIME32_3;
    h32 ^= h32 >> 16;
    return h32;
}

/* ── Main ──────────────────────────────────────────────────────────────────*/
int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <seed> <num_frames> <frame_size> <output.zst> [flags...]\n"
            "\n"
            "Flags:\n"
            "  --seekable          append seekable format footer\n"
            "  --no-content-size   omit content size from frame headers\n"
            "  --checksum          add checksum to seekable entries\n"
            "  --level N           compression level (default: 1)\n"
            "  --vary-size         vary frame sizes\n"
            "  --dump-raw FILE     write raw uncompressed data to FILE\n",
            argv[0]);
        return 1;
    }

    uint64_t base_seed   = strtoull(argv[1], NULL, 10);
    uint32_t num_frames  = (uint32_t)strtoul(argv[2], NULL, 10);
    size_t   frame_size  = (size_t)strtoull(argv[3], NULL, 10);
    const char *out_path = argv[4];

    /* Parse flags */
    int seekable       = 0;
    int no_content_size = 0;
    int checksum_flag  = 0;
    int level          = 1;
    int vary_size      = 0;
    const char *raw_path = NULL;

    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--seekable") == 0)         seekable = 1;
        else if (strcmp(argv[i], "--no-content-size") == 0) no_content_size = 1;
        else if (strcmp(argv[i], "--checksum") == 0)    checksum_flag = 1;
        else if (strcmp(argv[i], "--vary-size") == 0)   vary_size = 1;
        else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc)
            level = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dump-raw") == 0 && i + 1 < argc)
            raw_path = argv[++i];
        else {
            fprintf(stderr, "gen_seekable: unknown flag '%s'\n", argv[i]);
            return 1;
        }
    }

    if (num_frames == 0) {
        fprintf(stderr, "gen_seekable: num_frames must be > 0\n");
        return 1;
    }

    /* Open output file */
    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "gen_seekable: cannot open '%s': %s\n", out_path, strerror(errno));
        return 1;
    }

    /* Open raw output file if requested */
    FILE *fraw = NULL;
    if (raw_path) {
        fraw = fopen(raw_path, "wb");
        if (!fraw) {
            fprintf(stderr, "gen_seekable: cannot open '%s': %s\n", raw_path, strerror(errno));
            fclose(fout);
            return 1;
        }
    }

    /* Allocate per-frame tracking for seekable footer */
    uint32_t *comp_sizes   = NULL;
    uint32_t *decomp_sizes = NULL;
    uint32_t *checksums    = NULL;
    if (seekable) {
        comp_sizes   = calloc(num_frames, sizeof(uint32_t));
        decomp_sizes = calloc(num_frames, sizeof(uint32_t));
        if (checksum_flag)
            checksums = calloc(num_frames, sizeof(uint32_t));
    }

    /* Create compression context */
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (!cctx) {
        fprintf(stderr, "gen_seekable: ZSTD_createCCtx failed\n");
        fclose(fout);
        if (fraw) fclose(fraw);
        return 1;
    }

    /* Allocate buffers */
    size_t max_frame_size = frame_size;
    if (vary_size) max_frame_size = frame_size; /* max is frame_size when vary */
    size_t comp_buf_size = ZSTD_compressBound(max_frame_size);
    uint8_t *raw_buf  = malloc(max_frame_size);
    uint8_t *comp_buf = malloc(comp_buf_size);
    if (!raw_buf || !comp_buf) {
        fprintf(stderr, "gen_seekable: malloc failed\n");
        free(raw_buf);
        free(comp_buf);
        free(comp_sizes);
        free(decomp_sizes);
        free(checksums);
        ZSTD_freeCCtx(cctx);
        fclose(fout);
        if (fraw) fclose(fraw);
        return 1;
    }

    /* Compress each frame */
    for (uint32_t i = 0; i < num_frames; i++) {
        size_t this_frame_size = frame_size;
        if (vary_size) {
            this_frame_size = (size_t)((uint64_t)frame_size * (i + 1) / num_frames);
            if (this_frame_size == 0) this_frame_size = 1;
        }

        /* Generate deterministic data */
        uint64_t frame_seed = base_seed + i;
        gen_frame_data(raw_buf, this_frame_size, frame_seed);

        /* Write raw data if requested */
        if (fraw) {
            if (fwrite(raw_buf, 1, this_frame_size, fraw) != this_frame_size) {
                fprintf(stderr, "gen_seekable: raw write error\n");
                return 1;
            }
        }

        /* Set compression parameters */
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
        if (no_content_size) {
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);
        }

        /* Compress */
        size_t comp_size = ZSTD_compress2(cctx, comp_buf, comp_buf_size,
                                          raw_buf, this_frame_size);
        if (ZSTD_isError(comp_size)) {
            fprintf(stderr, "gen_seekable: compression error: %s\n",
                    ZSTD_getErrorName(comp_size));
            return 1;
        }

        /* Write compressed frame */
        if (fwrite(comp_buf, 1, comp_size, fout) != comp_size) {
            fprintf(stderr, "gen_seekable: write error\n");
            return 1;
        }

        /* Track sizes for seekable footer */
        if (seekable) {
            comp_sizes[i]   = (uint32_t)comp_size;
            decomp_sizes[i] = (uint32_t)this_frame_size;
            if (checksum_flag) {
                checksums[i] = xxh32(raw_buf, this_frame_size);
            }
        }
    }

    /* Write seekable format footer if requested */
    if (seekable) {
        uint32_t size_per_entry = 8 + (checksum_flag ? 4 : 0);
        uint32_t table_size = size_per_entry * num_frames;
        uint32_t skippable_payload_size = table_size + 9; /* entries + footer */

        /* Skippable frame header: magic (0x184D2A5E) + payload size */
        write_le32(fout, 0x184D2A5E);
        write_le32(fout, skippable_payload_size);

        /* Frame entries */
        for (uint32_t i = 0; i < num_frames; i++) {
            write_le32(fout, comp_sizes[i]);
            write_le32(fout, decomp_sizes[i]);
            if (checksum_flag) {
                write_le32(fout, checksums[i]);
            }
        }

        /* Seek table footer: numFrames(4) + sfd(1) + magic(4) */
        write_le32(fout, num_frames);
        uint8_t sfd = checksum_flag ? 0x80 : 0x00;
        fwrite(&sfd, 1, 1, fout);
        write_le32(fout, 0x8F92EAB1);

        free(comp_sizes);
        free(decomp_sizes);
        free(checksums);
    }

    /* Cleanup */
    ZSTD_freeCCtx(cctx);
    free(raw_buf);
    free(comp_buf);

    if (fclose(fout) != 0) {
        fprintf(stderr, "gen_seekable: close error: %s\n", strerror(errno));
        return 1;
    }
    if (fraw && fclose(fraw) != 0) {
        fprintf(stderr, "gen_seekable: raw close error: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}
