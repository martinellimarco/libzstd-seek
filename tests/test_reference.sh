#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# test_reference.sh — Cross-validate libzstd-seek against the zstd CLI
#
# Usage:
#   test_reference.sh TEST_SEEK GEN_SEEKABLE BLOBS_DIR SEED NUM_FRAMES FRAME_SIZE [GEN_FLAGS...]
#
# Generates a multi-frame .zst file, decompresses it with BOTH the library
# and the zstd CLI, and verifies byte-for-byte agreement through multiple
# access patterns:
#   1. Library sequential read  vs  zstd CLI output
#   2. 10 000 random seeks      vs  zstd CLI output
#   3. Full backward traversal  vs  zstd CLI output
#   4. Chunked reads (256 B)    vs  zstd CLI output
#   5. Sequential seek (small files only)

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

# ── Check zstd CLI availability ────────────────────────────────────────
if ! command -v zstd >/dev/null 2>&1; then
    log_skip "zstd CLI not found in PATH"
    exit 77   # CTest SKIP_RETURN_CODE
fi

# ── Create unique working directory ────────────────────────────────────
WORK_DIR=$(mktemp -d "${BLOBS_DIR}/ref_XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

ZST_FILE="$WORK_DIR/test.zst"
PRNG_RAW="$WORK_DIR/test.prng.raw"
CLI_RAW="$WORK_DIR/test.cli.raw"

# ── Generate test data ─────────────────────────────────────────────────
log_step "Generating: seed=$SEED frames=$NUM_FRAMES frame_size=$FRAME_SIZE ${GEN_FLAGS[*]:-}"
"$GEN_SEEKABLE" "$SEED" "$NUM_FRAMES" "$FRAME_SIZE" "$ZST_FILE" \
    --dump-raw "$PRNG_RAW" "${GEN_FLAGS[@]}"

RAW_SIZE=$(wc -c < "$PRNG_RAW" | tr -d ' ')
ZST_SIZE=$(wc -c < "$ZST_FILE" | tr -d ' ')
log_info "Raw: ${RAW_SIZE} bytes  Compressed: ${ZST_SIZE} bytes"

# ── Step 0: Decompress with zstd CLI ──────────────────────────────────
log_step "zstd -d: decompressing"
if ! zstd -d -f -o "$CLI_RAW" "$ZST_FILE" 2>/dev/null; then
    log_fail "zstd -d failed"
    exit 1
fi
log_pass "zstd -d succeeded"

# ── Step 0b: Sanity check — CLI output must match PRNG reference ──────
log_step "Sanity: zstd CLI output vs PRNG reference"
if ! cmp -s "$CLI_RAW" "$PRNG_RAW"; then
    CLI_SIZE=$(wc -c < "$CLI_RAW" | tr -d ' ')
    log_fail "zstd CLI output (${CLI_SIZE} B) differs from PRNG reference (${RAW_SIZE} B)"
    cmp "$CLI_RAW" "$PRNG_RAW" 2>&1 || true
    exit 1
fi
log_pass "zstd CLI output == PRNG reference (${RAW_SIZE} bytes)"

# ── From here on, all library checks use cli.raw as the reference ─────
REF="$CLI_RAW"

# ── Step A: read_all (sequential full read) ───────────────────────────
log_step "Library: read_all vs CLI reference"
run "$TEST_SEEK" read_all "$ZST_FILE" "$REF"
if ! assert_rc 0; then
    log_fail "read_all: library output differs from zstd CLI"
    exit 1
fi
log_pass "read_all: library == zstd CLI"

# ── Step B: seek_random (10 000 random seek+read ops) ─────────────────
log_step "Library: seek_random (10000 ops) vs CLI reference"
run "$TEST_SEEK" seek_random "$ZST_FILE" "$REF" "$SEED" 10000
if ! assert_rc 0; then
    log_fail "seek_random: library output differs from zstd CLI"
    exit 1
fi
log_pass "seek_random: 10000 random positions verified against zstd CLI"

# ── Step C: seek_set_backward (full reverse traversal) ────────────────
if [ "$RAW_SIZE" -le 65536 ]; then
    log_step "Library: seek_set_backward vs CLI reference"
    run "$TEST_SEEK" seek_set_backward "$ZST_FILE" "$REF"
    if ! assert_rc 0; then
        log_fail "seek_set_backward: library output differs from zstd CLI"
        exit 1
    fi
    log_pass "seek_set_backward: all ${RAW_SIZE} positions verified (reverse)"
else
    log_skip "seek_set_backward (file too large: ${RAW_SIZE} bytes, limit 65536)"
fi

# ── Step D: read_chunks (256-byte chunks) ─────────────────────────────
log_step "Library: read_chunks (256 B) vs CLI reference"
run "$TEST_SEEK" read_chunks "$ZST_FILE" "$REF" 256
if ! assert_rc 0; then
    log_fail "read_chunks: library output differs from zstd CLI"
    exit 1
fi
log_pass "read_chunks: chunked reads verified against zstd CLI"

# ── Step E: seek_set_sequential (every byte, small files only) ────────
if [ "$RAW_SIZE" -le 8192 ]; then
    log_step "Library: seek_set_sequential vs CLI reference"
    run "$TEST_SEEK" seek_set_sequential "$ZST_FILE" "$REF"
    if ! assert_rc 0; then
        log_fail "seek_set_sequential: library output differs from zstd CLI"
        exit 1
    fi
    log_pass "seek_set_sequential: all ${RAW_SIZE} positions verified (forward)"
else
    log_skip "seek_set_sequential (file too large: ${RAW_SIZE} bytes, limit 8192)"
fi

# ── Summary ───────────────────────────────────────────────────────────
log_info "All checks agree: Library == zstd CLI for all access patterns"
print_summary
