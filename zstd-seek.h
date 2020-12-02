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

/* Structs */

typedef struct{
    uint32_t frameIdx;     //the frame index
    size_t compressedPos;  //where this frame begin in the compressed stream
    size_t uncompressedPos;//the equivalent position in the uncompressed stream
} ZSTDSeek_JumpTableRecord;

typedef struct{
    ZSTDSeek_JumpTableRecord *records;
    uint32_t length;
    uint32_t capacity;
    size_t uncompressedFileSize; //to support SEEK_END
} ZSTDSeek_JumpTable;

typedef struct ZSTDSeek_Context_s ZSTDSeek_Context;

/* Jump Table API */

/*
Returns a pointer to the jump table used by the given context, 0 if the context is not valid.
*/
ZSTDSeek_JumpTable* ZSTDSeek_getJumpTableOfContext(ZSTDSeek_Context *sctx);

/*
 These are for advanced use. Don't use them unless you understand exactly what you are doing.
 */
ZSTDSeek_JumpTable* ZSTDSeek_newJumpTable();
void ZSTDSeek_freeJumpTable(ZSTDSeek_JumpTable* jt);
void ZSTDSeek_addJumpTableRecord(ZSTDSeek_JumpTable* jt, uint32_t frameIdx, size_t compressedPos, size_t uncompressedPos);
int ZSTDSeek_initializeJumpTable(ZSTDSeek_Context *sctx, void *buff, size_t size);

/* Seek API */

/*
Create a ZSTDSeek_Context from a file.
file is the path to the file
On success returns a valid ZSTDSeek_Context.
Returns ZSTDSEEK_CREATE_OPEN_FAILED if the file can't be opened.
Returns ZSTDSEEK_CREATE_MAP_FAILED if the file can't be memory mapped.
*/
ZSTDSeek_Context* ZSTDSeek_createFromFile(const char* file);

/*
Create a ZSTDSeek_Context from a buffer.
buff is a buffer with the whole zstd data, eg a memory mapped file.
length is the length of buff.
Returns 0 in case of failure.
*/
ZSTDSeek_Context* ZSTDSeek_create(void *buff, size_t size);

/*
It reads outBuffSize bytes of uncompressed data from the sctx context buffer into outBuff.
Returns the number of bytes read.
*/
size_t ZSTDSeek_read(void *outBuff, size_t outBuffSize, ZSTDSeek_Context *sctx);

/*
Like fseek.
Origin can be SEEK_SET, SEEK_END or SEEK_CUR.
Returns 0 if seek was successfull.
*/
int ZSTDSeek_seek(ZSTDSeek_Context *sctx, long offset, int origin);

/*
 Like ftell.
 Returns the current position in the uncompressed file or -1 in case of failure.
 */
long ZSTDSeek_tell(ZSTDSeek_Context *sctx);

/*
Free the context.
*/
void ZSTDSeek_free(ZSTDSeek_Context *sctx);

#if defined (__cplusplus)
}
#endif

#endif
