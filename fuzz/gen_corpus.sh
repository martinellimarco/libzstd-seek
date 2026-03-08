#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# gen_corpus.sh — Generate seed corpus for libzstd-seek fuzz harnesses
#
# Usage:
#   bash gen_corpus.sh <corpus_decompress_dir> <corpus_seekable_dir> <corpus_jumptable_dir>

set -euo pipefail

ZSTD="${ZSTD:-zstd}"

CORPUS_DECOMPRESS="${1:?Usage: $0 <corpus_decompress> <corpus_seekable> <corpus_jumptable>}"
CORPUS_SEEKABLE="${2:?Usage: $0 <corpus_decompress> <corpus_seekable> <corpus_jumptable>}"
CORPUS_JUMPTABLE="${3:?Usage: $0 <corpus_decompress> <corpus_seekable> <corpus_jumptable>}"

mkdir -p "$CORPUS_DECOMPRESS" "$CORPUS_SEEKABLE" "$CORPUS_JUMPTABLE"

# ── Helper: create a minimal zstd-compressed file ────────────────────────────
make_zst() {
    local content="$1"
    local output="$2"
    printf '%s' "$content" | "$ZSTD" -f -1 --no-check -o "$output" 2>/dev/null
}

# ── Helper: create a multi-frame zstd file ───────────────────────────────────
make_multiframe_zst() {
    local output="$1"
    shift
    > "$output"
    for content in "$@"; do
        printf '%s' "$content" | "$ZSTD" -f -1 --no-check >> "$output" 2>/dev/null
    done
}

# ── Helper: write LE32 to a file ─────────────────────────────────────────────
write_le32() {
    local val="$1"
    local file="$2"
    printf "\\x$(printf '%02x' $((val & 0xFF)))\\x$(printf '%02x' $(((val >> 8) & 0xFF)))\\x$(printf '%02x' $(((val >> 16) & 0xFF)))\\x$(printf '%02x' $(((val >> 24) & 0xFF)))" >> "$file"
}

# ══════════════════════════════════════════════════════════════════════════════
# Decompress corpus seeds
# ══════════════════════════════════════════════════════════════════════════════
echo "Generating decompress corpus seeds..."

# Single frame, tiny
make_zst "Hello" "$CORPUS_DECOMPRESS/single_tiny.zst"

# Single frame, medium
make_zst "$(printf '%0256d' 0)" "$CORPUS_DECOMPRESS/single_256.zst"

# Single frame, larger (1KB)
make_zst "$(head -c 1024 /dev/urandom | base64)" "$CORPUS_DECOMPRESS/single_1k.zst"

# Multi-frame (4 frames)
make_multiframe_zst "$CORPUS_DECOMPRESS/multi_4.zst" "AAAA" "BBBB" "CCCC" "DDDD"

# Multi-frame (10 frames)
make_multiframe_zst "$CORPUS_DECOMPRESS/multi_10.zst" \
    "frame0" "frame1" "frame2" "frame3" "frame4" \
    "frame5" "frame6" "frame7" "frame8" "frame9"

# Multi-frame with varying sizes
make_multiframe_zst "$CORPUS_DECOMPRESS/multi_vary.zst" \
    "A" "BB" "CCC" "DDDD" "EEEEE" "FFFFFF" "GGGGGGG" "HHHHHHHH"

# Single frame without content-size (--no-check already omits checksum)
printf '%0256d' 0 | "$ZSTD" -f -1 --no-check --no-content-size -o "$CORPUS_DECOMPRESS/no_content_size.zst" 2>/dev/null || true

# Empty-ish: just the zstd magic (invalid but interesting for parser)
printf '\x28\xb5\x2f\xfd' > "$CORPUS_DECOMPRESS/magic_only.bin"

# Random bytes (not valid zstd)
dd if=/dev/urandom of="$CORPUS_DECOMPRESS/random_256.bin" bs=256 count=1 2>/dev/null

# Truncated: valid frame header but cut short
make_zst "Hello World" "$CORPUS_DECOMPRESS/full.zst"
dd if="$CORPUS_DECOMPRESS/full.zst" of="$CORPUS_DECOMPRESS/truncated.zst" bs=8 count=1 2>/dev/null
rm -f "$CORPUS_DECOMPRESS/full.zst"

# Valid frame + garbage at end (exercises mixed format handling)
make_zst "Hello" "$CORPUS_DECOMPRESS/frame_plus_garbage.zst"
dd if=/dev/urandom bs=32 count=1 2>/dev/null >> "$CORPUS_DECOMPRESS/frame_plus_garbage.zst"

echo "  → $(ls "$CORPUS_DECOMPRESS" | wc -l | tr -d ' ') seeds in $CORPUS_DECOMPRESS"

# ══════════════════════════════════════════════════════════════════════════════
# Seekable corpus seeds
# ══════════════════════════════════════════════════════════════════════════════
echo "Generating seekable corpus seeds..."

# Valid seekable format footer (1 frame: compressed=14, decompressed=5)
# Format: skippable_header(8) + entry(8) + footer(9) = 25 bytes
# Skippable magic: 0x184D2A5E, payload size: 17
# Entry: comp_size=14 (LE32), decomp_size=5 (LE32)
# Footer: numFrames=1 (LE32), sfd=0x00 (1 byte), magic=0x8F92EAB1 (LE32)
printf '\x5e\x2a\x4d\x18' > "$CORPUS_SEEKABLE/valid_footer.bin"     # skippable magic
printf '\x11\x00\x00\x00' >> "$CORPUS_SEEKABLE/valid_footer.bin"    # payload size = 17
printf '\x0e\x00\x00\x00' >> "$CORPUS_SEEKABLE/valid_footer.bin"    # comp_size = 14
printf '\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/valid_footer.bin"    # decomp_size = 5
printf '\x01\x00\x00\x00' >> "$CORPUS_SEEKABLE/valid_footer.bin"    # numFrames = 1
printf '\x00'              >> "$CORPUS_SEEKABLE/valid_footer.bin"    # sfd = 0x00
printf '\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/valid_footer.bin"    # seekable magic

# Footer with checksum flag set
printf '\x5e\x2a\x4d\x18' > "$CORPUS_SEEKABLE/footer_checksum.bin"
printf '\x15\x00\x00\x00' >> "$CORPUS_SEEKABLE/footer_checksum.bin" # payload size = 21
printf '\x0e\x00\x00\x00' >> "$CORPUS_SEEKABLE/footer_checksum.bin" # comp_size
printf '\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/footer_checksum.bin" # decomp_size
printf '\xaa\xbb\xcc\xdd' >> "$CORPUS_SEEKABLE/footer_checksum.bin" # checksum
printf '\x01\x00\x00\x00' >> "$CORPUS_SEEKABLE/footer_checksum.bin" # numFrames = 1
printf '\x80'              >> "$CORPUS_SEEKABLE/footer_checksum.bin" # sfd = 0x80 (checksum)
printf '\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/footer_checksum.bin" # seekable magic

# Multi-frame seekable footer (4 frames)
: > "$CORPUS_SEEKABLE/multi_footer.bin"
printf '\x5e\x2a\x4d\x18' >> "$CORPUS_SEEKABLE/multi_footer.bin"   # skippable magic
printf '\x29\x00\x00\x00' >> "$CORPUS_SEEKABLE/multi_footer.bin"   # payload = 41 = 4*8+9
# Entry 1
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/multi_footer.bin"
# Entry 2
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/multi_footer.bin"
# Entry 3
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/multi_footer.bin"
# Entry 4
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/multi_footer.bin"
# Footer
printf '\x04\x00\x00\x00' >> "$CORPUS_SEEKABLE/multi_footer.bin"   # numFrames = 4
printf '\x00'              >> "$CORPUS_SEEKABLE/multi_footer.bin"   # sfd = 0
printf '\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/multi_footer.bin"   # seekable magic

# Malformed: wrong seekable magic
printf '\x5e\x2a\x4d\x18\x11\x00\x00\x00' > "$CORPUS_SEEKABLE/bad_magic.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/bad_magic.bin"
printf '\x01\x00\x00\x00\x00\xde\xad\xbe\xef' >> "$CORPUS_SEEKABLE/bad_magic.bin"

# Malformed: reserved bits set in sfd
printf '\x5e\x2a\x4d\x18\x11\x00\x00\x00' > "$CORPUS_SEEKABLE/reserved_bits.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/reserved_bits.bin"
printf '\x01\x00\x00\x00\x7c\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/reserved_bits.bin"

# Malformed: wrong skippable magic (0x184D2A50 instead of 0x184D2A5E)
printf '\x50\x2a\x4d\x18\x11\x00\x00\x00' > "$CORPUS_SEEKABLE/bad_skip_magic.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/bad_skip_magic.bin"
printf '\x01\x00\x00\x00\x00\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/bad_skip_magic.bin"

# Malformed: frame size mismatch (payload says 0xFF but real data is smaller)
printf '\x5e\x2a\x4d\x18\xff\x00\x00\x00' > "$CORPUS_SEEKABLE/bad_frame_size.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/bad_frame_size.bin"
printf '\x01\x00\x00\x00\x00\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/bad_frame_size.bin"

# Footer with numFrames=0
printf '\x5e\x2a\x4d\x18\x09\x00\x00\x00' > "$CORPUS_SEEKABLE/zero_frames.bin"
printf '\x00\x00\x00\x00\x00\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/zero_frames.bin"

# Footer with very large numFrames (0xFFFFFFFF)
printf '\x5e\x2a\x4d\x18\x11\x00\x00\x00' > "$CORPUS_SEEKABLE/huge_numframes.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/huge_numframes.bin"
printf '\xff\xff\xff\xff\x00\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/huge_numframes.bin"

# Random bytes (pure fuzz seed)
dd if=/dev/urandom of="$CORPUS_SEEKABLE/random_64.bin" bs=64 count=1 2>/dev/null
dd if=/dev/urandom of="$CORPUS_SEEKABLE/random_256.bin" bs=256 count=1 2>/dev/null

echo "  → $(ls "$CORPUS_SEEKABLE" | wc -l | tr -d ' ') seeds in $CORPUS_SEEKABLE"

# ══════════════════════════════════════════════════════════════════════════════
# Jumptable corpus seeds
# ══════════════════════════════════════════════════════════════════════════════
echo "Generating jumptable corpus seeds..."

# Monotonically increasing records (valid pattern)
# 4 records: (0,0), (14,5), (28,10), (42,15)
: > "$CORPUS_JUMPTABLE/valid_records.bin"
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$CORPUS_JUMPTABLE/valid_records.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_JUMPTABLE/valid_records.bin"
printf '\x1c\x00\x00\x00\x0a\x00\x00\x00' >> "$CORPUS_JUMPTABLE/valid_records.bin"
printf '\x2a\x00\x00\x00\x0f\x00\x00\x00' >> "$CORPUS_JUMPTABLE/valid_records.bin"

# Single record (edge case)
printf '\x00\x00\x00\x00\x00\x00\x00\x00' > "$CORPUS_JUMPTABLE/single_record.bin"

# Descending records (invalid order, tests robustness)
: > "$CORPUS_JUMPTABLE/descending.bin"
printf '\x2a\x00\x00\x00\x0f\x00\x00\x00' >> "$CORPUS_JUMPTABLE/descending.bin"
printf '\x1c\x00\x00\x00\x0a\x00\x00\x00' >> "$CORPUS_JUMPTABLE/descending.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_JUMPTABLE/descending.bin"

# Duplicate records
: > "$CORPUS_JUMPTABLE/duplicates.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_JUMPTABLE/duplicates.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_JUMPTABLE/duplicates.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_JUMPTABLE/duplicates.bin"

# Zero-valued records
: > "$CORPUS_JUMPTABLE/zeros.bin"
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$CORPUS_JUMPTABLE/zeros.bin"
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$CORPUS_JUMPTABLE/zeros.bin"

# Large values
: > "$CORPUS_JUMPTABLE/large_values.bin"
printf '\xff\xff\xff\x7f\xff\xff\xff\x7f' >> "$CORPUS_JUMPTABLE/large_values.bin"
printf '\xff\xff\xff\xff\xff\xff\xff\xff' >> "$CORPUS_JUMPTABLE/large_values.bin"

# Random bytes
dd if=/dev/urandom of="$CORPUS_JUMPTABLE/random_128.bin" bs=128 count=1 2>/dev/null
dd if=/dev/urandom of="$CORPUS_JUMPTABLE/random_512.bin" bs=512 count=1 2>/dev/null

echo "  → $(ls "$CORPUS_JUMPTABLE" | wc -l | tr -d ' ') seeds in $CORPUS_JUMPTABLE"

echo ""
echo "Done. Seed corpus generated."
echo ""
echo "Run fuzzers with dictionary for best results:"
echo "  ./fuzz_decompress corpus_decompress/ -dict=zstd_seek.dict -max_len=65536 -timeout=10"
echo "  ./fuzz_seekable   corpus_seekable/   -dict=zstd_seek.dict -max_len=4096  -timeout=5"
echo "  ./fuzz_jumptable  corpus_jumptable/  -dict=zstd_seek.dict -max_len=65536 -timeout=10"
