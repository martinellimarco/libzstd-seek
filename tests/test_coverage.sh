#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# test_coverage.sh — Generate LLVM coverage report for libzstd-seek
#
# Usage:
#   bash tests/test_coverage.sh <build_dir>
#
# Merges the .profraw files produced by a -DCOVERAGE=ON build and generates
# an HTML report.  On macOS, the report is opened automatically.

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <build_dir>"
    echo "  build_dir: path to the cmake build directory (with .profraw files)"
    exit 1
fi

BUILD_DIR="$1"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# ── Locate LLVM tools ────────────────────────────────────────────────────────
# Search order:
#   1. Homebrew LLVM — /opt/homebrew/opt/llvm/bin (unversioned)
#   2. Homebrew LLVM — /opt/homebrew/opt/llvm@*/bin (versioned, highest first)
#   3. Xcode toolchain (via xcrun) — ships with Xcode Command Line Tools
#   4. System PATH
find_tool() {
    local tool="$1"
    # 1) Homebrew unversioned
    local brew_path="/opt/homebrew/opt/llvm/bin/$tool"
    if [ -x "$brew_path" ]; then
        echo "$brew_path"
        return
    fi
    # 2) Homebrew versioned (llvm@18, llvm@19, llvm@21, …) — pick highest
    local versioned
    versioned="$(ls -d /opt/homebrew/opt/llvm@*/bin/"$tool" 2>/dev/null \
                 | sort -t@ -k2 -rn | head -1)" || true
    if [ -n "$versioned" ] && [ -x "$versioned" ]; then
        echo "$versioned"
        return
    fi
    # 3) Xcode toolchain
    if command -v xcrun >/dev/null 2>&1; then
        local xc_path
        xc_path="$(xcrun -f "$tool" 2>/dev/null)" || true
        if [ -n "$xc_path" ] && [ -x "$xc_path" ]; then
            echo "$xc_path"
            return
        fi
    fi
    # 4) System PATH
    command -v "$tool" 2>/dev/null || true
}

PROFDATA="$(find_tool llvm-profdata)"
COV="$(find_tool llvm-cov)"

if [ -z "$PROFDATA" ] || [ -z "$COV" ]; then
    echo "ERROR: llvm-profdata and llvm-cov are required."
    echo "  macOS: brew install llvm  (or install Xcode Command Line Tools)"
    echo "  Linux: apt install llvm"
    exit 1
fi

echo "Using: $PROFDATA"
echo "Using: $COV"

# ── Merge profiles ───────────────────────────────────────────────────────────
PROFRAW_FILES=$(find "$BUILD_DIR" -name 'cov_*.profraw' 2>/dev/null)
if [ -z "$PROFRAW_FILES" ]; then
    echo "ERROR: No .profraw files found in $BUILD_DIR"
    echo "Did you build with -DCOVERAGE=ON and run the tests?"
    exit 1
fi

MERGED="$BUILD_DIR/coverage.profdata"
echo ""
echo "Merging profraw files..."
# shellcheck disable=SC2086
"$PROFDATA" merge -sparse $PROFRAW_FILES -o "$MERGED"

# ── Locate the test binary (for coverage instrumentation) ────────────────────
TEST_BIN="$BUILD_DIR/tests/test_seek"
if [ ! -x "$TEST_BIN" ]; then
    echo "ERROR: test_seek binary not found at $TEST_BIN"
    exit 1
fi

# ── Console report ───────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════"
echo "  Coverage Report"
echo "═══════════════════════════════════════════════════"
"$COV" report "$TEST_BIN" \
    --instr-profile="$MERGED" \
    --sources "$SRC_DIR/zstd-seek.c"

# ── HTML report ──────────────────────────────────────────────────────────────
HTML_DIR="$SRC_DIR/tests/coverage/html"
mkdir -p "$HTML_DIR"

"$COV" show "$TEST_BIN" \
    --instr-profile="$MERGED" \
    --sources "$SRC_DIR/zstd-seek.c" \
    --format=html \
    --output-dir="$HTML_DIR"

echo ""
echo "HTML report: $HTML_DIR/index.html"

# Auto-open on macOS
if [ "$(uname -s)" = "Darwin" ] && [ -f "$HTML_DIR/index.html" ]; then
    open "$HTML_DIR/index.html" 2>/dev/null || true
fi
