#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# gen_corpus.sh — Generate seed corpus for libzstd-seek fuzz harnesses
#
# Usage:
#   bash gen_corpus.sh <corpus_decompress_dir> <corpus_seekable_dir>

set -euo pipefail

CORPUS_DECOMPRESS="${1:?Usage: $0 <corpus_decompress_dir> <corpus_seekable_dir>}"
CORPUS_SEEKABLE="${2:?Usage: $0 <corpus_decompress_dir> <corpus_seekable_dir>}"

mkdir -p "$CORPUS_DECOMPRESS" "$CORPUS_SEEKABLE"

# ── Helper: create a minimal zstd-compressed file ────────────────────────────
make_zst() {
    local content="$1"
    local output="$2"
    printf '%s' "$content" | zstd -1 --no-check -o "$output" 2>/dev/null
}

# ── Helper: create a multi-frame zstd file ───────────────────────────────────
make_multiframe_zst() {
    local output="$1"
    shift
    > "$output"
    for content in "$@"; do
        printf '%s' "$content" | zstd -1 --no-check >> "$output" 2>/dev/null
    done
}

# ── Decompress corpus seeds ──────────────────────────────────────────────────
echo "Generating decompress corpus seeds..."

# Single frame, tiny
make_zst "Hello" "$CORPUS_DECOMPRESS/single_tiny.zst"

# Single frame, medium
make_zst "$(printf '%0256d' 0)" "$CORPUS_DECOMPRESS/single_256.zst"

# Multi-frame (4 frames)
make_multiframe_zst "$CORPUS_DECOMPRESS/multi_4.zst" "AAAA" "BBBB" "CCCC" "DDDD"

# Multi-frame (10 frames)
make_multiframe_zst "$CORPUS_DECOMPRESS/multi_10.zst" \
    "frame0" "frame1" "frame2" "frame3" "frame4" \
    "frame5" "frame6" "frame7" "frame8" "frame9"

# Empty-ish: just the zstd magic (invalid but interesting for parser)
printf '\x28\xb5\x2f\xfd' > "$CORPUS_DECOMPRESS/magic_only.bin"

# Random bytes (not valid zstd)
dd if=/dev/urandom of="$CORPUS_DECOMPRESS/random_256.bin" bs=256 count=1 2>/dev/null

# Truncated: valid frame header but cut short
make_zst "Hello World" "$CORPUS_DECOMPRESS/full.zst"
dd if="$CORPUS_DECOMPRESS/full.zst" of="$CORPUS_DECOMPRESS/truncated.zst" bs=8 count=1 2>/dev/null
rm -f "$CORPUS_DECOMPRESS/full.zst"

echo "  → $(ls "$CORPUS_DECOMPRESS" | wc -l | tr -d ' ') seeds in $CORPUS_DECOMPRESS"

# ── Seekable corpus seeds ────────────────────────────────────────────────────
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

# Malformed: wrong seekable magic
printf '\x5e\x2a\x4d\x18\x11\x00\x00\x00' > "$CORPUS_SEEKABLE/bad_magic.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/bad_magic.bin"
printf '\x01\x00\x00\x00\x00\xde\xad\xbe\xef' >> "$CORPUS_SEEKABLE/bad_magic.bin"

# Malformed: reserved bits set in sfd
printf '\x5e\x2a\x4d\x18\x11\x00\x00\x00' > "$CORPUS_SEEKABLE/reserved_bits.bin"
printf '\x0e\x00\x00\x00\x05\x00\x00\x00' >> "$CORPUS_SEEKABLE/reserved_bits.bin"
printf '\x01\x00\x00\x00\x7c\xb1\xea\x92\x8f' >> "$CORPUS_SEEKABLE/reserved_bits.bin"

# Random bytes (pure fuzz seed)
dd if=/dev/urandom of="$CORPUS_SEEKABLE/random_64.bin" bs=64 count=1 2>/dev/null
dd if=/dev/urandom of="$CORPUS_SEEKABLE/random_256.bin" bs=256 count=1 2>/dev/null

echo "  → $(ls "$CORPUS_SEEKABLE" | wc -l | tr -d ' ') seeds in $CORPUS_SEEKABLE"

echo ""
echo "Done. Seed corpus generated."
