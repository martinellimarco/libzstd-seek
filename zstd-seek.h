/* ******************************************************************
 * libzstd-seek
 * Copyright (c) 2020, Martinelli Marco
 *
 * You can contact the author at :
 * - Source repository : https://github.com/martinellimarco/libzstd-seek
 *
 * This source code is licensed under the GPLv3 (found in the LICENSE
 * file in the root directory of this source tree).
****************************************************************** */

#ifndef _ZSTD_SEEK_
#define _ZSTD_SEEK_

#if defined (__cplusplus)
extern "C" {
#endif

#include <stdio.h>
#include <zstd.h>

#ifndef _ZSTD_SEEK_DEBUG_
#define _ZSTD_SEEK_DEBUG_ 0
#endif

#define DEBUG(...) \
do { \
    if (_ZSTD_SEEK_DEBUG_){ \
        flockfile(stderr); \
        fprintf(stderr, "%s(): ", __func__); \
        fprintf(stderr, __VA_ARGS__); \
        funlockfile(stderr); \
    } \
} while (0)

/* Error constants */
#define ZSTDSEEK_ERR_NEGATIVE_SEEK -1
#define ZSTDSEEK_ERR_BEYOND_END_SEEK -2
#define ZSTDSEEK_ERR_READ -3

/* Seekable format constants */
#define ZSTD_SEEK_TABLE_FOOTER_SIZE 9
#define ZSTD_SEEKABLE_MAGICNUMBER 0x8F92EAB1
#define ZSTD_SKIPPABLE_HEADER_SIZE 8

/* Structs */

typedef struct{
    size_t compressedPos;  //where this frame begin in the compressed stream
    size_t uncompressedPos;//the equivalent position in the uncompressed stream
} ZSTDSeek_JumpTableRecord;

typedef struct{
    ZSTDSeek_JumpTableRecord *records;
    uint32_t length;
    uint32_t capacity;
} ZSTDSeek_JumpTable;

typedef struct ZSTDSeek_Context_s ZSTDSeek_Context;

/* Jump Table API */

/*
 * Returns a pointer to the jump table used by the given context, 0 if the context is not valid.
 */
ZSTDSeek_JumpTable* ZSTDSeek_getJumpTableOfContext(ZSTDSeek_Context *sctx);

/*
 * These are for advanced use. Don't use them unless you understand exactly what you are doing.
 */
ZSTDSeek_JumpTable* ZSTDSeek_newJumpTable();
void ZSTDSeek_freeJumpTable(ZSTDSeek_JumpTable* jt);

/*
 * Add a new record to the jump table jt. It's a simple map between compressed and uncompressed positions.
 * Uncompressed positions must be at the start of a frame.
 * The last record is special. The compressedPos is the compressed file size and the uncompressedPos is the uncompressed file size.
 */
void ZSTDSeek_addJumpTableRecord(ZSTDSeek_JumpTable* jt, size_t compressedPos, size_t uncompressedPos);

/*
 * Parse the file and fill the jump table. Don't use in combination with addJumpTableRecord.
 * You don't need to call this unless you used a create*WithoutJumpTable method to get the context.
 */
int ZSTDSeek_initializeJumpTable(ZSTDSeek_Context *sctx);

/*
 * Parse the file and fill the jump table up until a certain uncompressed position. Don't use in combination with addJumpTableRecord.
 * You don't need to call this unless you used a create*WithoutJumpTable method to get the context.
 */
int ZSTDSeek_initializeJumpTableUpUntilPos(ZSTDSeek_Context *sctx, size_t upUntilPos);

/*
 * Return 1 if the jump table is fully initialized, 0 otherwise.
 */
int ZSTDSeek_jumpTableIsInitialized(ZSTDSeek_Context *sctx);

/* Seek API */

/*
 * Create a ZSTDSeek_Context from a file and initialize the jump table.
 * file is the path to the file
 * On success returns a valid ZSTDSeek_Context.
 * Returns ZSTDSEEK_CREATE_OPEN_FAILED if the file can't be opened.
 * Returns ZSTDSEEK_CREATE_MAP_FAILED if the file can't be memory mapped.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFile(const char* file);

/*
 * Create a ZSTDSeek_Context from a file but does not initialze the jump table.
 * You'll have to call ZSTDSeek_initializeJumpTable manually or add records with ZSTDSeek_addJumpTableRecord.
 * file is the path to the file
 * On success returns a valid ZSTDSeek_Context.
 * Returns ZSTDSEEK_CREATE_OPEN_FAILED if the file can't be opened.
 * Returns ZSTDSEEK_CREATE_MAP_FAILED if the file can't be memory mapped.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFileWithoutJumpTable(const char* file);

/*
 * Create a ZSTDSeek_Context from a buffer and initialize the jump table.
 * buff is a buffer with the whole zstd data, eg a memory mapped file.
 * length is the length of buff.
 * Returns 0 in case of failure.
 */
ZSTDSeek_Context* ZSTDSeek_create(void *buff, size_t size);

/*
 * Create a ZSTDSeek_Context from a buffer but does not initialze the jump table.
 * You'll have to call ZSTDSeek_initializeJumpTable manually or add records with ZSTDSeek_addJumpTableRecord.
 * buff is a buffer with the whole zstd data, eg a memory mapped file.
 * length is the length of buff.
 * Returns 0 in case of failure.
 */
ZSTDSeek_Context* ZSTDSeek_createWithoutJumpTable(void *buff, size_t size);

/*
 * Create a ZSTDSeek_Context from a file descriptor but does not initialze the jump table.
 * You'll have to call ZSTDSeek_initializeJumpTable manually or add records with ZSTDSeek_addJumpTableRecord.
 * USE WITH CAUTION. The file descriptor will be used with mmap.
 * fd is the file descriptor
 * On success returns a valid ZSTDSeek_Context.
 * Returns ZSTDSEEK_CREATE_MAP_FAILED if the fd can't be memory mapped.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptorWithoutJumpTable(int fd);

/*
 * Create a ZSTDSeek_Context from a file descriptor and initialize the jump table.
 * USE WITH CAUTION. The file descriptor will be used with mmap.
 * fd is the file descriptor
 * On success returns a valid ZSTDSeek_Context.
 * Returns ZSTDSEEK_CREATE_MAP_FAILED if the fd can't be memory mapped.
 */
ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptor(int fd);

/*
 * It reads outBuffSize bytes of uncompressed data from the sctx context buffer into outBuff.
 * Returns the number of bytes read.
 */
size_t ZSTDSeek_read(void *outBuff, size_t outBuffSize, ZSTDSeek_Context *sctx);

/*
 * Like fseek.
 * Origin can be SEEK_SET, SEEK_END or SEEK_CUR.
 * Returns 0 if seek was successfull.
 */
int ZSTDSeek_seek(ZSTDSeek_Context *sctx, long offset, int origin);

/*
 * Like ftell.
 * Returns the current position in the uncompressed file or -1 in case of failure.
 */
long ZSTDSeek_tell(ZSTDSeek_Context *sctx);

/*
 * Like ftell.
 * Returns the current position in the compressed file or -1 in case of failure.
 */
long ZSTDSeek_compressedTell(ZSTDSeek_Context *sctx);

/*
 * Return the size of the uncompressed file. It will trigger the initialization of the full jump table.
 */
size_t ZSTDSeek_uncompressedFileSize(ZSTDSeek_Context *sctx);

/*
 * Return the last known size of the uncompressed file. It's the size of all the uncompressed frames discovered so far stored in the jump table.
 */
size_t ZSTDSeek_lastKnownUncompressedFileSize(ZSTDSeek_Context *sctx);

/*
 * Return the file descriptor associated with the memory mapped file if available, -1 otherwise.
 * USE WITH CAUTION. IF YOU DON'T KNOW WHY YOU NEED IT YOU DON'T NEED IT.
 */
int ZSTDSeek_fileno(ZSTDSeek_Context *sctx);

/*
 * Returns the number of frames in the file.
 */
size_t ZSTDSeek_getNumberOfFrames(ZSTDSeek_Context *sctx);

/*
 * Returns 1 if there are more than one frame in the file.
 */
int ZSTDSeek_isMultiframe(ZSTDSeek_Context *sctx);

/*
 * Free the context.
 */
void ZSTDSeek_free(ZSTDSeek_Context *sctx);

#if defined (__cplusplus)
}
#endif

#endif
