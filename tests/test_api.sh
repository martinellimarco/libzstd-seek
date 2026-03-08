#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# test_api.sh — API-specific tests for libzstd-seek
#
# Usage:
#   test_api.sh TEST_SEEK GEN_SEEKABLE BLOBS_DIR TEST_NAME
#
# Prepares the appropriate test data and invokes test_seek with the test name.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/helpers.sh"

TEST_SEEK="$1"
GEN_SEEKABLE="$2"
BLOBS_DIR="$3"
TEST_NAME="$4"

# ── Create unique working directory ──────────────────────────────────────────
WORK_DIR=$(mktemp -d "${BLOBS_DIR}/api_XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

# ── Standard test files ──────────────────────────────────────────────────────
# Most API tests use the same 4-frame seekable file
gen_standard() {
    local flags=("$@")
    "$GEN_SEEKABLE" 42 4 1024 "$WORK_DIR/test.zst" \
        --dump-raw "$WORK_DIR/test.raw" "${flags[@]}"
}

gen_multiframe() {
    "$GEN_SEEKABLE" 42 10 1000 "$WORK_DIR/multi.zst" \
        --dump-raw "$WORK_DIR/multi.raw" --seekable
}

gen_single_frame() {
    "$GEN_SEEKABLE" 42 1 4096 "$WORK_DIR/single.zst" \
        --dump-raw "$WORK_DIR/single.raw"
}

# ── Dispatch ─────────────────────────────────────────────────────────────────
case "$TEST_NAME" in
    # ── Create variants ──────────────────────────────────────────────────────
    create_from_file|create_from_buffer|create_from_fd)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    create_from_file_no_jt|create_from_buffer_no_jt|create_from_fd_no_jt)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Seek tests ───────────────────────────────────────────────────────────
    seek_set_sequential|seek_set_backward|seek_cur_forward)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    seek_cur_backward)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    seek_end)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    seek_random)
        gen_multiframe
        log_step "$TEST_NAME (10000 ops, SET/CUR/END)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/multi.zst" "$WORK_DIR/multi.raw" 7 10000
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    seek_out_of_file)
        gen_standard --seekable
        log_step "$TEST_NAME (9 boundary cases)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Read pattern: read_too_much ───────────────────────────────────────────
    read_too_much)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Jump table tests ─────────────────────────────────────────────────────
    jump_table_auto)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" 4
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    jump_table_progressive)
        gen_multiframe
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/multi.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    jump_table_new_free)
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    jump_table_manual)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    jt_progressive_reads)
        # Non-seekable format so JT grows incrementally via reads (not from footer)
        "$GEN_SEEKABLE" 42 10 1000 "$WORK_DIR/progressive.zst" \
            --dump-raw "$WORK_DIR/progressive.raw"
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/progressive.zst" "$WORK_DIR/progressive.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Info function tests ──────────────────────────────────────────────────
    file_size)
        gen_standard --seekable
        # 4 frames × 1024 bytes = 4096 bytes total
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" 4096
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    last_known_size)
        gen_multiframe
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/multi.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    frame_count)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" 4
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    is_multiframe)
        gen_standard --seekable
        log_step "$TEST_NAME (multiframe=1)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" 1
        assert_rc 0 || exit 1

        gen_single_frame
        log_step "$TEST_NAME (multiframe=0)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/single.zst" 0
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    fileno_check)
        gen_standard --seekable
        log_step "$TEST_NAME"
        # fileno is checked inside create_from_fd
        run "$TEST_SEEK" create_from_fd "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    compressed_tell)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Read pattern tests ───────────────────────────────────────────────────
    read_byte_by_byte)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    read_chunks)
        gen_standard --seekable
        log_step "$TEST_NAME (128-byte chunks)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw" 128
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    frame_boundary)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    single_frame)
        gen_single_frame
        log_step "$TEST_NAME"
        run "$TEST_SEEK" read_all "$WORK_DIR/single.zst" "$WORK_DIR/single.raw"
        assert_rc 0 || exit 1
        run "$TEST_SEEK" seek_random "$WORK_DIR/single.zst" "$WORK_DIR/single.raw" 99 500
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    large_read)
        gen_standard --seekable
        log_step "$TEST_NAME"
        # read_all already handles reading the entire file, which is a "large read"
        run "$TEST_SEEK" read_all "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Seekable format tests ────────────────────────────────────────────────
    seekable_basic)
        # Generate with seekable format
        "$GEN_SEEKABLE" 42 4 1024 "$WORK_DIR/seekable.zst" \
            --dump-raw "$WORK_DIR/seekable.raw" --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" read_all "$WORK_DIR/seekable.zst" "$WORK_DIR/seekable.raw"
        assert_rc 0 || exit 1
        run "$TEST_SEEK" seek_random "$WORK_DIR/seekable.zst" "$WORK_DIR/seekable.raw" 77 5000
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    seekable_checksum)
        "$GEN_SEEKABLE" 42 4 1024 "$WORK_DIR/checksum.zst" \
            --dump-raw "$WORK_DIR/checksum.raw" --seekable --checksum
        log_step "$TEST_NAME"
        run "$TEST_SEEK" read_all "$WORK_DIR/checksum.zst" "$WORK_DIR/checksum.raw"
        assert_rc 0 || exit 1
        run "$TEST_SEEK" seek_random "$WORK_DIR/checksum.zst" "$WORK_DIR/checksum.raw" 77 5000
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    seekable_vs_scan)
        # Both seekable and non-seekable should produce same results
        "$GEN_SEEKABLE" 42 4 1024 "$WORK_DIR/s.zst" --dump-raw "$WORK_DIR/data.raw" --seekable
        "$GEN_SEEKABLE" 42 4 1024 "$WORK_DIR/ns.zst"
        log_step "$TEST_NAME"
        run "$TEST_SEEK" read_all "$WORK_DIR/s.zst" "$WORK_DIR/data.raw"
        assert_rc 0 || exit 1
        run "$TEST_SEEK" read_all "$WORK_DIR/ns.zst" "$WORK_DIR/data.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME: seekable and non-seekable produce same data"
        ;;

    seekable_malformed_footer)
        "$GEN_SEEKABLE" 42 4 1024 "$WORK_DIR/seekable.zst" \
            --dump-raw "$WORK_DIR/seekable.raw" --seekable
        log_step "$TEST_NAME (3 corruption variants)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/seekable.zst" "$WORK_DIR/seekable.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Edge case tests ─────────────────────────────────────────────────────
    seek_cur_zero)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    seek_to_same_pos)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst" "$WORK_DIR/test.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    fileno_buffer)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    read_zero_bytes)
        gen_standard --seekable
        log_step "$TEST_NAME"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/test.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    compressed_tell_absolute)
        # Single large frame to stress the decompression loop
        "$GEN_SEEKABLE" 42 1 1048576 "$WORK_DIR/large1f.zst" \
            --dump-raw "$WORK_DIR/large1f.raw"
        log_step "$TEST_NAME (1 frame × 1 MiB, absolute value check)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/large1f.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── compressedTell coverage tests ──────────────────────────────────────
    compressed_tell_monotonic)
        gen_multiframe
        log_step "$TEST_NAME (10-byte chunks, monotonicity)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/multi.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    compressed_tell_seek)
        gen_multiframe
        log_step "$TEST_NAME (JT boundary coherence)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/multi.zst"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    # ── Large forward seek test ────────────────────────────────────────────
    seek_forward_large)
        gen_multiframe
        log_step "$TEST_NAME (+500 SEEK_CUR within frames)"
        run "$TEST_SEEK" "$TEST_NAME" "$WORK_DIR/multi.zst" "$WORK_DIR/multi.raw"
        assert_rc 0 || exit 1
        log_pass "$TEST_NAME"
        ;;

    *)
        echo "Unknown test: $TEST_NAME" >&2
        exit 1
        ;;
esac
