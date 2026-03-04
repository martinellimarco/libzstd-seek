#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# test_error_paths.sh — Error path tests for libzstd-seek
#
# Usage:
#   test_error_paths.sh TEST_SEEK GEN_SEEKABLE BLOBS_DIR TEST_NAME

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/helpers.sh"

TEST_SEEK="$1"
GEN_SEEKABLE="$2"
BLOBS_DIR="$3"
TEST_NAME="$4"

# ── Create unique working directory ──────────────────────────────────────────
WORK_DIR=$(mktemp -d "${BLOBS_DIR}/err_XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

# ── Generate a valid test file for error tests that need one ─────────────────
gen_valid() {
    "$GEN_SEEKABLE" 42 4 1024 "$WORK_DIR/valid.zst" --seekable
}

case "$TEST_NAME" in
    error_null)
        log_step "error_null"
        run "$TEST_SEEK" error_null
        assert_rc 0 || exit 1
        log_pass "error_null"
        ;;

    error_empty_file)
        # Create a truly empty file
        : > "$WORK_DIR/empty.zst"
        log_step "error_empty_file"
        run "$TEST_SEEK" error_empty_file "$WORK_DIR/empty.zst"
        assert_rc 0 || exit 1
        log_pass "error_empty_file"
        ;;

    error_truncated)
        # Create a truncated zstd file (first 15 bytes of a valid file)
        gen_valid
        dd if="$WORK_DIR/valid.zst" of="$WORK_DIR/truncated.zst" bs=15 count=1 2>/dev/null
        log_step "error_truncated"
        run "$TEST_SEEK" error_truncated "$WORK_DIR/truncated.zst"
        assert_rc 0 || exit 1
        log_pass "error_truncated"
        ;;

    error_invalid_format)
        # Create a file with non-zstd content
        echo "This is not a zstd file" > "$WORK_DIR/invalid.txt"
        log_step "error_invalid_format"
        run "$TEST_SEEK" error_invalid_format "$WORK_DIR/invalid.txt"
        assert_rc 0 || exit 1
        log_pass "error_invalid_format"
        ;;

    error_seek_negative)
        gen_valid
        log_step "error_seek_negative"
        run "$TEST_SEEK" error_seek_negative "$WORK_DIR/valid.zst"
        assert_rc 0 || exit 1
        log_pass "error_seek_negative"
        ;;

    error_seek_beyond)
        gen_valid
        log_step "error_seek_beyond"
        run "$TEST_SEEK" error_seek_beyond "$WORK_DIR/valid.zst"
        assert_rc 0 || exit 1
        log_pass "error_seek_beyond"
        ;;

    error_seek_invalid_origin)
        gen_valid
        log_step "error_seek_invalid_origin"
        run "$TEST_SEEK" error_seek_invalid_origin "$WORK_DIR/valid.zst"
        assert_rc 0 || exit 1
        log_pass "error_seek_invalid_origin"
        ;;

    error_read_past_eof)
        gen_valid
        log_step "error_read_past_eof"
        run "$TEST_SEEK" error_read_past_eof "$WORK_DIR/valid.zst"
        assert_rc 0 || exit 1
        log_pass "error_read_past_eof"
        ;;

    error_corrupted_header)
        gen_valid
        log_step "error_corrupted_header"
        run "$TEST_SEEK" error_corrupted_header "$WORK_DIR/valid.zst"
        assert_rc 0 || exit 1
        log_pass "error_corrupted_header"
        ;;

    error_mixed_format)
        gen_valid
        log_step "error_mixed_format"
        run "$TEST_SEEK" error_mixed_format "$WORK_DIR/valid.zst"
        assert_rc 0 || exit 1
        log_pass "error_mixed_format"
        ;;

    *)
        echo "Unknown error test: $TEST_NAME" >&2
        exit 1
        ;;
esac
