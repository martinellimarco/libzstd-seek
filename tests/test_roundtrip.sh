#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# test_roundtrip.sh — Round-trip test for libzstd-seek
#
# Usage:
#   test_roundtrip.sh TEST_SEEK GEN_SEEKABLE BLOBS_DIR SEED NUM_FRAMES FRAME_SIZE [GEN_FLAGS...]
#
# Generates a multi-frame .zst file, reads it back through the library,
# and verifies the data matches the original.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/helpers.sh"

TEST_SEEK="$1"
GEN_SEEKABLE="$2"
BLOBS_DIR="$3"
SEED="$4"
NUM_FRAMES="$5"
FRAME_SIZE="$6"
shift 6
GEN_FLAGS=("$@")

# ── Create unique working directory ──────────────────────────────────────────
WORK_DIR=$(mktemp -d "${BLOBS_DIR}/rt_XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

ZST_FILE="$WORK_DIR/test.zst"
RAW_FILE="$WORK_DIR/test.raw"

# ── Generate test data ───────────────────────────────────────────────────────
log_step "Generating: seed=$SEED frames=$NUM_FRAMES frame_size=$FRAME_SIZE ${GEN_FLAGS[*]:-}"
"$GEN_SEEKABLE" "$SEED" "$NUM_FRAMES" "$FRAME_SIZE" "$ZST_FILE" \
    --dump-raw "$RAW_FILE" "${GEN_FLAGS[@]}"

RAW_SIZE=$(wc -c < "$RAW_FILE" | tr -d ' ')
ZST_SIZE=$(wc -c < "$ZST_FILE" | tr -d ' ')
log_info "Raw: ${RAW_SIZE} bytes, Compressed: ${ZST_SIZE} bytes"

# ── Test: read_all ───────────────────────────────────────────────────────────
log_step "read_all"
run "$TEST_SEEK" read_all "$ZST_FILE" "$RAW_FILE"
if ! assert_rc 0; then
    log_fail "read_all failed"
    exit 1
fi
log_pass "read_all"

# ── Test: read_byte_by_byte (only for small files) ──────────────────────────
if [ "$RAW_SIZE" -le 8192 ]; then
    log_step "read_byte_by_byte"
    run "$TEST_SEEK" read_byte_by_byte "$ZST_FILE" "$RAW_FILE"
    if ! assert_rc 0; then
        log_fail "read_byte_by_byte failed"
        exit 1
    fi
    log_pass "read_byte_by_byte"
else
    log_skip "read_byte_by_byte (file too large: $RAW_SIZE bytes)"
fi

# ── Test: read_chunks ────────────────────────────────────────────────────────
log_step "read_chunks (256 bytes)"
run "$TEST_SEEK" read_chunks "$ZST_FILE" "$RAW_FILE" 256
if ! assert_rc 0; then
    log_fail "read_chunks failed"
    exit 1
fi
log_pass "read_chunks"

# ── Test: seek_random ────────────────────────────────────────────────────────
log_step "seek_random (1000 ops)"
run "$TEST_SEEK" seek_random "$ZST_FILE" "$RAW_FILE" "$SEED" 1000
if ! assert_rc 0; then
    log_fail "seek_random failed"
    exit 1
fi
log_pass "seek_random"

print_summary
