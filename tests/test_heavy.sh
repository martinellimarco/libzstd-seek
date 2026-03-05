#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# test_heavy.sh — Realistic tests with real data blobs and t2sz
#
# Usage:
#   test_heavy.sh TEST_SEEK BLOBS_DIR TEST_NAME
#
# Generates large, realistic data (text, binary, zeros, mixed), compresses
# with t2sz, decompresses with the library, and verifies SHA256 + byte-exact
# match + random seek correctness.
#
# Requirements:
#   - t2sz in PATH (skip if not found)
#   - ~1 GB free disk space
#
# Set SKIP_HEAVY_TESTS=1 to skip these tests in CI/CD.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/helpers.sh"

TEST_SEEK="$1"
BLOBS_DIR="$2"
TEST_NAME="$3"

# ── Skip conditions ──────────────────────────────────────────────────────
if [[ "${SKIP_HEAVY_TESTS:-}" == "1" ]]; then
    log_skip "SKIP_HEAVY_TESTS=1"
    exit 77
fi

if ! command -v t2sz &>/dev/null; then
    log_skip "t2sz not found in PATH"
    exit 77
fi

if ! check_disk_space "$BLOBS_DIR" 1; then
    log_skip "not enough disk space (need 1 GB)"
    exit 77
fi

# ── Setup ────────────────────────────────────────────────────────────────
WORK_DIR=$(mktemp -d "${BLOBS_DIR}/heavy_XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

RAW_FILE="$WORK_DIR/raw.dat"
ZST_FILE="$WORK_DIR/compressed.zst"
DEC_FILE="$WORK_DIR/decompressed.dat"

# ── Data generators ──────────────────────────────────────────────────────
# All generators produce exactly $size bytes in $RAW_FILE.
# They avoid pipe-based truncation (head -c / yes | head) to prevent
# SIGPIPE failures under set -o pipefail.

generate_text() {
    local size=$1
    log_step "Generating ${size} bytes of text (base64)"
    # Generate enough random bytes so base64 expansion (4/3 + newlines) exceeds $size
    local raw_bytes=$(( size * 3 / 4 + 1048576 ))
    dd if=/dev/urandom bs=1048576 count=$(( raw_bytes / 1048576 + 1 )) 2>/dev/null \
        | LC_ALL=C base64 > "$WORK_DIR/tmp_full"
    # Truncate from file (no SIGPIPE: head reads a regular file)
    head -c "$size" "$WORK_DIR/tmp_full" > "$RAW_FILE"
    rm -f "$WORK_DIR/tmp_full"
}

generate_binary() {
    local size=$1
    log_step "Generating ${size} bytes of random binary"
    dd if=/dev/urandom of="$RAW_FILE" bs=1048576 count=$((size / 1048576)) 2>/dev/null
}

generate_zeros() {
    local size=$1
    log_step "Generating ${size} bytes of zeros"
    dd if=/dev/zero of="$RAW_FILE" bs=1048576 count=$((size / 1048576)) 2>/dev/null
}

generate_repetitive() {
    local size=$1 output=$2
    # Fill with repeating 'A'-'Z','0'-'9' pattern using tr on /dev/zero
    dd if=/dev/zero bs=1048576 count=$((size / 1048576 + 1)) 2>/dev/null \
        | LC_ALL=C tr '\0' 'A' > "$WORK_DIR/tmp_rep"
    head -c "$size" "$WORK_DIR/tmp_rep" > "$output"
    rm -f "$WORK_DIR/tmp_rep"
}

generate_mixed() {
    local size=$1
    local third=$((size / 3))
    local rest=$((size - third - third))
    log_step "Generating ${size} bytes of mixed data (text+binary+pattern)"
    # 1/3 text
    generate_text "$third"
    cp "$RAW_FILE" "$WORK_DIR/mix_part"
    # 1/3 binary
    dd if=/dev/urandom bs=1048576 count=$((third / 1048576)) 2>/dev/null >> "$WORK_DIR/mix_part"
    # 1/3 repetitive pattern
    generate_repetitive "$rest" "$WORK_DIR/tmp_rep_chunk"
    cat "$WORK_DIR/tmp_rep_chunk" >> "$WORK_DIR/mix_part"
    rm -f "$WORK_DIR/tmp_rep_chunk"
    mv "$WORK_DIR/mix_part" "$RAW_FILE"
}

# ── Compress + verify ────────────────────────────────────────────────────

compress_and_verify() {
    local label="$1"
    local frame_size="$2"
    local level="$3"

    local raw_size
    raw_size=$(wc -c < "$RAW_FILE" | tr -d ' ')
    local raw_hash
    raw_hash=$(sha256_file "$RAW_FILE")
    log_info "Raw: ${raw_size} bytes  SHA256: ${raw_hash}"

    # ── Compress with t2sz ───────────────────────────────────────────
    log_step "Compressing with t2sz -r -s ${frame_size} -l ${level}"
    t2sz -r -s "$frame_size" -l "$level" -f -o "$ZST_FILE" "$RAW_FILE"
    local zst_size
    zst_size=$(wc -c < "$ZST_FILE" | tr -d ' ')
    log_info "Compressed: ${zst_size} bytes  (ratio: $(echo "scale=2; $raw_size / $zst_size" | bc)x)"

    # ── SHA256 verification: decompress → hash → compare ─────────────
    log_step "SHA256 verification (decompress → hash)"
    run "$TEST_SEEK" decompress "$ZST_FILE" "$DEC_FILE"
    if ! assert_rc 0; then
        log_fail "${label}: decompress failed"
        return 1
    fi
    local dec_hash
    dec_hash=$(sha256_file "$DEC_FILE")
    if [[ "$dec_hash" != "$raw_hash" ]]; then
        log_fail "${label}: SHA256 mismatch! expected ${raw_hash}, got ${dec_hash}"
        return 1
    fi
    log_pass "${label}: SHA256 match (${raw_hash:0:16}...)"
    rm -f "$DEC_FILE"

    # ── Byte-exact verification (read_all) ───────────────────────────
    log_step "Byte-exact verification (read_all)"
    run "$TEST_SEEK" read_all "$ZST_FILE" "$RAW_FILE"
    if ! assert_rc 0; then
        log_fail "${label}: read_all failed"
        return 1
    fi
    log_pass "${label}: read_all byte-exact"

    # ── Random seek verification (10,000 ops) ────────────────────────
    log_step "Random seek verification (10,000 ops)"
    run "$TEST_SEEK" seek_random "$ZST_FILE" "$RAW_FILE" 42 10000
    if ! assert_rc 0; then
        log_fail "${label}: seek_random failed"
        return 1
    fi
    log_pass "${label}: seek_random 10K ops"

    # ── Chunked read verification (4 KB chunks) ─────────────────────
    log_step "Chunked read verification (4 KB chunks)"
    run "$TEST_SEEK" read_chunks "$ZST_FILE" "$RAW_FILE" 4096
    if ! assert_rc 0; then
        log_fail "${label}: read_chunks failed"
        return 1
    fi
    log_pass "${label}: read_chunks 4KB"
}

# ── Test cases ───────────────────────────────────────────────────────────

case "$TEST_NAME" in
    heavy_text)
        generate_text $((10 * 1048576))
        compress_and_verify "text 10MB / 256K frames / level 3" 256K 3
        ;;

    heavy_binary)
        generate_binary $((10 * 1048576))
        compress_and_verify "binary 10MB / 256K frames / level 3" 256K 3
        ;;

    heavy_zeros)
        generate_zeros $((50 * 1048576))
        compress_and_verify "zeros 50MB / 1M frames / level 3" 1M 3
        ;;

    heavy_mixed)
        generate_mixed $((20 * 1048576))
        compress_and_verify "mixed 20MB / 512K frames / level 9" 512K 9
        ;;

    heavy_level_max)
        generate_text $((5 * 1048576))
        compress_and_verify "text 5MB / 256K frames / level 22" 256K 22
        ;;

    heavy_small_frames)
        generate_binary $((5 * 1048576))
        compress_and_verify "binary 5MB / 4K frames / level 1" 4K 1
        ;;

    heavy_single_frame)
        generate_binary $((20 * 1048576))
        compress_and_verify "binary 20MB / single frame / level 3" 20M 3
        ;;

    *)
        echo "Unknown heavy test: $TEST_NAME" >&2
        exit 1
        ;;
esac

print_summary
