// SPDX-License-Identifier: GPL-3.0-or-later OR MIT

/* ******************************************************************
 * libzstd-seek
 * Copyright (c) 2020, Martinelli Marco
 *
 * You can contact the author at :
 * - Source repository : https://github.com/martinellimarco/libzstd-seek
 *
 * This source code is licensed under both the MIT license (found in
 * the LICENSE file) and the GPLv3 (found in the COPYING file).
****************************************************************** */

#ifndef _ZSTD_SEEK_
#define _ZSTD_SEEK_

#if defined (__cplusplus)
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <zstd.h>

#ifndef _ZSTD_SEEK_DEBUG_
#define _ZSTD_SEEK_DEBUG_ 0
#endif

#ifdef _WIN32
#define DEBUG(...)
#else
#define DEBUG(...) \
do { \
    if (_ZSTD_SEEK_DEBUG_){ \
        flockfile(stderr); \
        fprintf(stderr, "%s(): ", __func__); \
        fprintf(stderr, __VA_ARGS__); \
        funlockfile(stderr); \
    } \
} while (0)
#endif

/** @name Error constants
 *  Negative error codes returned by ZSTDSeek_seek() and ZSTDSeek_read().
 *  @{
 */
#define ZSTDSEEK_ERR_NEGATIVE_SEEK (-1)   /**< Resolved seek position is negative. */
#define ZSTDSEEK_ERR_BEYOND_END_SEEK (-2) /**< Resolved seek position exceeds file size. */
#define ZSTDSEEK_ERR_READ (-3)            /**< Decompression or stream I/O error. */
/** @} */

/** @name Seekable format constants
 *  @{
 */
#define ZSTD_SEEK_TABLE_FOOTER_SIZE 9
#define ZSTD_SEEKABLE_MAGICNUMBER 0x8F92EAB1
#define ZSTD_SKIPPABLE_HEADER_SIZE 8
/** @} */

/* ── Structs ────────────────────────────────────────────────────── */

/** A single jump table record mapping a compressed position to an
 *  uncompressed position.  Both positions refer to frame boundaries. */
typedef struct{
    size_t compressedPos;   /**< Byte offset where the frame begins in the compressed stream. */
    size_t uncompressedPos; /**< Equivalent byte offset in the uncompressed stream. */
} ZSTDSeek_JumpTableRecord;

/** Array of jump table records used for random access into a multi-frame
 *  zstd stream.  The last record is a sentinel whose @c compressedPos is the
 *  total compressed size and whose @c uncompressedPos is the total
 *  uncompressed size. */
typedef struct{
    ZSTDSeek_JumpTableRecord *records; /**< Dynamically allocated array of records. */
    uint64_t length;                   /**< Number of records currently stored. */
    uint64_t capacity;                 /**< Allocated capacity (in number of records). */
} ZSTDSeek_JumpTable;

/** Opaque context for seekable decompression. */
typedef struct ZSTDSeek_Context_s ZSTDSeek_Context;

/* ── Jump Table API ─────────────────────────────────────────────── */

/**
 * Return the jump table used by the given context.
 *
 * The returned pointer is owned by the context and must not be freed
 * separately.  It remains valid until ZSTDSeek_free() is called.
 *
 * @param sctx  Context to query.
 * @return Pointer to the jump table, or @c NULL if @p sctx is @c NULL.
 */
ZSTDSeek_JumpTable* ZSTDSeek_getJumpTableOfContext(ZSTDSeek_Context *sctx);

/**
 * Allocate a new, empty jump table.
 *
 * This is intended for advanced use only.  In most cases the jump table
 * is created automatically by ZSTDSeek_create() or
 * ZSTDSeek_initializeJumpTable().
 *
 * @return A newly allocated jump table.  The caller must free it with
 *         ZSTDSeek_freeJumpTable().
 */
ZSTDSeek_JumpTable* ZSTDSeek_newJumpTable();

/**
 * Free a jump table previously allocated with ZSTDSeek_newJumpTable().
 *
 * @param jt  Jump table to free.  May be @c NULL (no-op).
 */
void ZSTDSeek_freeJumpTable(ZSTDSeek_JumpTable* jt);

/**
 * Append a record to the jump table.
 *
 * Records must be added in monotonically increasing order of both
 * @p compressedPos and @p uncompressedPos.  Each record maps the start
 * of a compressed frame to the corresponding uncompressed offset.
 *
 * The last record is special: its @p compressedPos is the total compressed
 * size and its @p uncompressedPos is the total uncompressed size (sentinel).
 *
 * Do not mix manual record insertion with ZSTDSeek_initializeJumpTable()
 * or ZSTDSeek_initializeJumpTableUpUntilPos().
 *
 * @param jt              Jump table to modify.
 * @param compressedPos   Byte offset of the frame in the compressed stream.
 * @param uncompressedPos Byte offset of the frame in the uncompressed stream.
 */
void ZSTDSeek_addJumpTableRecord(ZSTDSeek_JumpTable* jt, size_t compressedPos, size_t uncompressedPos);

/**
 * Parse the compressed data and fully populate the jump table.
 *
 * If the data ends with a valid seekable format footer the table is built
 * from the footer metadata; otherwise every frame is scanned sequentially.
 *
 * This function is a no-op if the jump table is already fully initialized.
 *
 * You only need to call this if the context was created with one of the
 * @c create*WithoutJumpTable functions.  Do not combine with manual
 * ZSTDSeek_addJumpTableRecord() calls.
 *
 * @param sctx  Context whose jump table should be populated.
 * @return 0 on success, -1 on error (invalid data or @p sctx is @c NULL).
 */
int32_t ZSTDSeek_initializeJumpTable(ZSTDSeek_Context *sctx);

/**
 * Parse the compressed data and populate the jump table up to a given
 * uncompressed position.
 *
 * Scanning stops as soon as a frame whose uncompressed end position is
 * at or beyond @p upUntilPos has been processed.  Subsequent calls
 * continue from where the previous one left off.
 *
 * You only need to call this if the context was created with one of the
 * @c create*WithoutJumpTable functions.  Do not combine with manual
 * ZSTDSeek_addJumpTableRecord() calls.
 *
 * @param sctx        Context whose jump table should be populated.
 * @param upUntilPos  Target uncompressed position (inclusive).
 * @return 0 on success, -1 on error (invalid data or @p sctx is @c NULL).
 */
int32_t ZSTDSeek_initializeJumpTableUpUntilPos(ZSTDSeek_Context *sctx, size_t upUntilPos);

/**
 * Check whether the jump table has been fully initialized.
 *
 * @param sctx  Context to query.
 * @return true if fully initialized, false otherwise.
 */
bool ZSTDSeek_jumpTableIsInitialized(const ZSTDSeek_Context *sctx);

/* ── Seek API — context creation ────────────────────────────────── */

/**
 * Create a context from a file path and initialize the jump table.
 *
 * The file is memory-mapped internally.  The mapping (and file descriptor)
 * is released when ZSTDSeek_free() is called.
 *
 * @param file  Path to a zstd-compressed file.
 * @return A new context on success, or @c NULL if the file cannot be
 *         opened, cannot be memory-mapped, or does not start with a valid
 *         zstd frame.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFile(const char* file);

/**
 * Create a context from a file path without initializing the jump table.
 *
 * After creation you must either call ZSTDSeek_initializeJumpTable() /
 * ZSTDSeek_initializeJumpTableUpUntilPos(), or manually populate the
 * jump table with ZSTDSeek_addJumpTableRecord().
 *
 * @param file  Path to a zstd-compressed file.
 * @return A new context on success, or @c NULL if the file cannot be
 *         opened, cannot be memory-mapped, or does not start with a valid
 *         zstd frame.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFileWithoutJumpTable(const char* file);

/**
 * Create a context from an in-memory buffer and initialize the jump table.
 *
 * The buffer must remain valid and unmodified for the lifetime of the
 * context.  Ownership is @b not transferred — the caller is responsible
 * for freeing @p buff after ZSTDSeek_free().
 *
 * @param buff  Pointer to the complete zstd-compressed data (e.g. a
 *              memory-mapped file).
 * @param size  Size of @p buff in bytes.
 * @return A new context on success, or @c NULL if @p buff does not start
 *         with a valid zstd frame or jump table initialization fails.
 */
ZSTDSeek_Context* ZSTDSeek_create(void *buff, size_t size);

/**
 * Create a context from an in-memory buffer without initializing the
 * jump table.
 *
 * After creation you must either call ZSTDSeek_initializeJumpTable() /
 * ZSTDSeek_initializeJumpTableUpUntilPos(), or manually populate the
 * jump table with ZSTDSeek_addJumpTableRecord().
 *
 * The buffer must remain valid and unmodified for the lifetime of the
 * context.  Ownership is @b not transferred.
 *
 * @param buff  Pointer to the complete zstd-compressed data.
 * @param size  Size of @p buff in bytes.
 * @return A new context on success, or @c NULL if @p buff does not start
 *         with a valid zstd frame.
 */
ZSTDSeek_Context* ZSTDSeek_createWithoutJumpTable(void *buff, size_t size);

/**
 * Create a context from a file descriptor without initializing the
 * jump table.
 *
 * The file descriptor is used with @c mmap.  The caller retains ownership
 * of @p fd — it is @b not closed by ZSTDSeek_free().
 *
 * After creation you must either call ZSTDSeek_initializeJumpTable() /
 * ZSTDSeek_initializeJumpTableUpUntilPos(), or manually populate the
 * jump table with ZSTDSeek_addJumpTableRecord().
 *
 * @warning Use with caution.  The file descriptor must refer to a regular
 *          file that supports @c mmap.
 *
 * @param fd  Open file descriptor for a zstd-compressed file.
 * @return A new context on success, or @c NULL if the descriptor cannot
 *         be memory-mapped or does not start with a valid zstd frame.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptorWithoutJumpTable(int32_t fd);

/**
 * Create a context from a file descriptor and initialize the jump table.
 *
 * The file descriptor is used with @c mmap.  The caller retains ownership
 * of @p fd — it is @b not closed by ZSTDSeek_free().
 *
 * @warning Use with caution.  The file descriptor must refer to a regular
 *          file that supports @c mmap.
 *
 * @param fd  Open file descriptor for a zstd-compressed file.
 * @return A new context on success, or @c NULL if the descriptor cannot
 *         be memory-mapped, does not start with a valid zstd frame, or
 *         jump table initialization fails.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptor(int32_t fd);

/* ── Seek API — I/O ─────────────────────────────────────────────── */

/**
 * Read uncompressed data from the current position.
 *
 * Behaves like @c fread: reads up to @p outBuffSize bytes of
 * uncompressed data into @p outBuff and advances the position
 * accordingly.  A short read (fewer bytes than requested) occurs at
 * frame boundaries or at the end of the uncompressed stream.
 *
 * @param outBuff     Destination buffer (must be at least @p outBuffSize bytes).
 * @param outBuffSize Maximum number of bytes to read.
 * @param sctx        Context to read from.
 * @return Number of bytes actually read (0 at EOF), or
 *         @ref ZSTDSEEK_ERR_READ on decompression failure or unreadable data.
 *         This is the only negative error code returned by this function;
 *         @ref ZSTDSEEK_ERR_NEGATIVE_SEEK and @ref ZSTDSEEK_ERR_BEYOND_END_SEEK
 *         are exclusive to ZSTDSeek_seek().
 */
int64_t ZSTDSeek_read(void *outBuff, size_t outBuffSize, ZSTDSeek_Context *sctx);

/**
 * Seek to a position in the uncompressed stream.
 *
 * Behaves like @c fseek.  Supported origins:
 *   - @c SEEK_SET — @p offset is an absolute uncompressed position.
 *   - @c SEEK_CUR — @p offset is relative to the current position.
 *   - @c SEEK_END — @p offset is relative to the end of the uncompressed
 *                    stream (typically negative or zero).
 *
 * @param sctx    Context to seek in.
 * @param offset  Byte offset (interpretation depends on @p origin).
 * @param origin  One of @c SEEK_SET, @c SEEK_CUR, or @c SEEK_END.
 * @return 0 on success,
 *         @ref ZSTDSEEK_ERR_NEGATIVE_SEEK if the resolved position is negative,
 *         @ref ZSTDSEEK_ERR_BEYOND_END_SEEK if the resolved position exceeds
 *         the uncompressed file size,
 *         @ref ZSTDSEEK_ERR_READ if a forward skip fails due to a
 *         decompression or stream error,
 *         or -1 for any other error (e.g. @c NULL context, invalid origin).
 */
int32_t ZSTDSeek_seek(ZSTDSeek_Context *sctx, int64_t offset, int32_t origin);

/**
 * Return the current position in the uncompressed stream.
 *
 * Behaves like @c ftell.
 *
 * @param sctx  Context to query.
 * @return Current uncompressed byte offset, or -1 if @p sctx is @c NULL.
 */
int64_t ZSTDSeek_tell(ZSTDSeek_Context *sctx);

/**
 * Return the current position in the compressed stream.
 *
 * @param sctx  Context to query.
 * @return Current compressed byte offset, or -1 if @p sctx is @c NULL.
 */
int64_t ZSTDSeek_compressedTell(ZSTDSeek_Context *sctx);

/* ── Seek API — info ────────────────────────────────────────────── */

/**
 * Return the total uncompressed size of the stream.
 *
 * On the first call this triggers full jump table initialization
 * (equivalent to calling ZSTDSeek_initializeJumpTable()).
 *
 * @param sctx  Context to query.
 * @return Total uncompressed size in bytes, or 0 if @p sctx is @c NULL.
 */
size_t ZSTDSeek_uncompressedFileSize(ZSTDSeek_Context *sctx);

/**
 * Return the uncompressed size known so far.
 *
 * This is the sum of uncompressed frame sizes discovered during jump
 * table construction up to this point.  Unlike
 * ZSTDSeek_uncompressedFileSize(), this does @b not trigger further
 * scanning.
 *
 * @param sctx  Context to query.
 * @return Last known uncompressed size in bytes, or 0 if @p sctx is
 *         @c NULL or no frames have been scanned yet.
 */
size_t ZSTDSeek_lastKnownUncompressedFileSize(ZSTDSeek_Context *sctx);

/**
 * Return the file descriptor associated with the underlying memory map.
 *
 * Only meaningful for contexts created from a file or file descriptor
 * (ZSTDSeek_createFromFile(), ZSTDSeek_createFromFileDescriptor(), etc.).
 * For buffer-based contexts (ZSTDSeek_create()) this always returns -1.
 *
 * @note Ownership depends on how the context was created:
 *       - **File-path** contexts (ZSTDSeek_createFromFile()): the context
 *         owns the descriptor and closes it in ZSTDSeek_free().
 *       - **File-descriptor** contexts (ZSTDSeek_createFromFileDescriptor()):
 *         the caller retains ownership.  Do not close the descriptor while
 *         the context is alive; close it yourself after ZSTDSeek_free().
 *
 * @param sctx  Context to query.
 * @return File descriptor, or -1 if unavailable.
 */
int32_t ZSTDSeek_fileno(const ZSTDSeek_Context *sctx);

/**
 * Return the total number of zstd frames (data + skippable) in the stream.
 *
 * This scans the compressed data from the beginning; the result is not
 * cached.
 *
 * @param sctx  Context to query.
 * @return Number of frames, or 0 if @p sctx is @c NULL.
 */
size_t ZSTDSeek_getNumberOfFrames(ZSTDSeek_Context *sctx);

/**
 * Check whether the stream contains more than one frame.
 *
 * Equivalent to <tt>ZSTDSeek_getNumberOfFrames(sctx) > 1</tt> but
 * short-circuits after finding two frames.
 *
 * @param sctx  Context to query.
 * @return @c true if the stream has two or more frames, @c false otherwise.
 */
bool ZSTDSeek_isMultiframe(ZSTDSeek_Context *sctx);

/* ── Seek API — cleanup ─────────────────────────────────────────── */

/**
 * Free a context and all associated resources.
 *
 * If the context was created from a file path, the underlying memory
 * mapping and file descriptor are released.  If it was created from a
 * user-supplied buffer, the buffer itself is @b not freed.
 *
 * Passing @c NULL is a safe no-op.
 *
 * @param sctx  Context to free.
 */
void ZSTDSeek_free(ZSTDSeek_Context *sctx);

#if defined (__cplusplus)
}
#endif

#endif
