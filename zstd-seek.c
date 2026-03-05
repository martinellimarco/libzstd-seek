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

#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <string.h>
#include "zstd-seek.h"

#ifdef _WIN32
#include "windows-mmap.h"
#else
#include <unistd.h>
#include <sys/mman.h>
#endif

typedef struct {
    size_t compressedOffset; //how may bytes to skip from the beginning of the compressed stream (skip to target frame)
    size_t uncompressedOffset; //how many bytes skip from the beginning of the uncompressed frame (move inside target frame)
    ZSTDSeek_JumpTableRecord jtr; //copy of the jump table record that was used to calculate this jump coordinate
} ZSTDSeek_JumpCoordinate;

struct ZSTDSeek_Context_s{
    ZSTD_DCtx* dctx;

    void *buff; //the start of the buffer with the zstd frame(s)
    size_t size; //the length of buff

    size_t lastFrameCompressedSize; //the size of the last frame processed by read

    size_t currentUncompressedPos; //the current position in the uncompressed file, returned by tell
    size_t currentCompressedPos; //the position in the compressed file, returned by compressedTell

    ZSTDSeek_JumpTable* jt;
    int32_t jumpTableFullyInitialized;

    ZSTDSeek_JumpCoordinate jc;

    size_t tmpOutBuffSize;
    uint8_t* tmpOutBuff;
    size_t tmpOutBuffPos; //the position where we read so far in the tmpOutBuff. if tmpOutBuffPos < output.pos we have data left in this buffer to read before we move on to uncompress more data

    int32_t mmap_fd; //the file descriptor of the memory map, used only if the context is created with ZSTDSeek_createFromFile or ZSTDSeek_createFromFileWithoutJumpTable
    int32_t close_fd; //1 if we own the mmap_fd and we are responsible of closing it, 0 otherwise

    uint8_t* inBuff; //it's a pointer to something inside buff
    ZSTD_inBuffer input;
    ZSTD_outBuffer output;
};

/* Jump Table API */

ZSTDSeek_JumpTable* ZSTDSeek_getJumpTableOfContext(ZSTDSeek_Context *sctx){
    if(!sctx){
        return NULL;
    }
    return sctx->jt;
}

ZSTDSeek_JumpTable* ZSTDSeek_newJumpTable(){
    ZSTDSeek_JumpTable *jt = malloc(sizeof(ZSTDSeek_JumpTable));
    if(!jt){
        return NULL;
    }

    jt->records = malloc(sizeof(ZSTDSeek_JumpTableRecord));
    if(!jt->records){
        free(jt);
        return NULL;
    }
    jt->length = 0;
    jt->capacity = 1;

    return jt;
}

void ZSTDSeek_freeJumpTable(ZSTDSeek_JumpTable* jt){
    if(!jt){
        return;
    }
    free(jt->records);
    free(jt);
}

/* Internal version that reports allocation failure via return code.
 * Returns 0 on success, -1 on failure. */
static int32_t addJumpTableRecord_(ZSTDSeek_JumpTable* jt, const size_t compressedPos, const size_t uncompressedPos){
    if(!jt){
        DEBUG("Invalid argument");
        return -1;
    }

    if(jt->length == jt->capacity){
        const uint64_t maxCapacity = SIZE_MAX / sizeof(ZSTDSeek_JumpTableRecord);
        if(jt->capacity >= maxCapacity){
            DEBUG("Jump Table: Maximum capacity reached\n");
            return -1;
        }
        uint64_t newCapacity = jt->capacity * 2;
        if(newCapacity > maxCapacity || newCapacity < jt->capacity){ /* overflow or exceed */
            newCapacity = maxCapacity;
        }
        ZSTDSeek_JumpTableRecord* newRecords = realloc(jt->records, (size_t)(newCapacity * sizeof(ZSTDSeek_JumpTableRecord)));
        if(!newRecords){
            DEBUG("Jump Table: Unable to allocate memory\n");
            return -1;
        }
        jt->records = newRecords;
        jt->capacity = newCapacity;
    }

    jt->records[jt->length++] = (ZSTDSeek_JumpTableRecord){
            compressedPos,
            uncompressedPos
    };
    return 0;
}

/* Public wrapper — keeps the void API for callers who don't check. */
void ZSTDSeek_addJumpTableRecord(ZSTDSeek_JumpTable* jt, const size_t compressedPos, const size_t uncompressedPos){
    addJumpTableRecord_(jt, compressedPos, uncompressedPos);
}

int32_t ZSTDSeek_initializeJumpTable(ZSTDSeek_Context *sctx){
    if(sctx && sctx->jumpTableFullyInitialized && sctx->jt->length > 0){
        return 0;
    }
    return ZSTDSeek_initializeJumpTableUpUntilPos(sctx, SIZE_MAX);
}

bool ZSTDSeek_isLittleEndian(){
    volatile int x = 1;
    return *(char*)(&x) == 1;
}

uint32_t ZSTDSeek_fromLE32(const uint32_t data){
    if(ZSTDSeek_isLittleEndian()){
        return data;
    }else{
        return ((data & 0xFF000000) >> 24) |
               ((data & 0x00FF0000) >> 8)  |
               ((data & 0x0000FF00) << 8)  |
               ((data & 0x000000FF) << 24);
    }
}

/* Safe unaligned load: read a little-endian uint32_t from a possibly
 * unaligned pointer via memcpy (well-defined in C99, unlike casting
 * to uint32_t*). */
static uint32_t load_le32(const void *ptr){
    uint32_t val;
    memcpy(&val, ptr, sizeof(val));
    return ZSTDSeek_fromLE32(val);
}

int32_t ZSTDSeek_initializeJumpTableUpUntilPos(ZSTDSeek_Context *sctx, const size_t upUntilPos){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }

    void *buff = sctx->buff;
    size_t size = sctx->size;

    if(size >= ZSTD_SEEK_TABLE_FOOTER_SIZE){
        uint8_t *footer = (uint8_t *)buff + (size - ZSTD_SEEK_TABLE_FOOTER_SIZE);
        const uint32_t magicnumber = load_le32(footer + 5);

        if(magicnumber == ZSTD_SEEKABLE_MAGICNUMBER){
            DEBUG("Seektable detected\n");
            const uint8_t sfd = *((uint8_t*)(footer + 4));
            const uint8_t checksumFlag = sfd >> 7;

            /* check reserved bits */
            if((sfd >> 2) & 0x1f){
                DEBUG("Last frame checksumFlag= %x: Bits 3-7 should be zero. Ignoring malformed seektable.\n",(uint32_t)sfd);
            }else{
                const uint32_t numFrames = load_le32(footer);
                const uint32_t sizePerEntry = 8 + (checksumFlag ? 4 : 0);

                /* Guard: numFrames must fit within the buffer space available for entries. */
                const size_t maxEntries = (size - ZSTD_SEEK_TABLE_FOOTER_SIZE - ZSTD_SKIPPABLE_HEADER_SIZE) / sizePerEntry;
                if(numFrames > maxEntries){
                    DEBUG("Seektable numFrames (%u) too large for buffer. Ignoring malformed seektable.\n", numFrames);
                }else{
                    const size_t tableSize = (size_t)sizePerEntry * numFrames;
                    const size_t frameSize = tableSize + ZSTD_SEEK_TABLE_FOOTER_SIZE + ZSTD_SKIPPABLE_HEADER_SIZE;

                    if(frameSize > size){
                        DEBUG("Seektable frame size (%zu) exceeds buffer size (%zu). Ignoring malformed seektable.\n", frameSize, size);
                    }else{
                        uint8_t *frame = (uint8_t *)buff + (size - frameSize);
                        const uint32_t skippableHeader = load_le32(frame);
                        if(skippableHeader != (ZSTD_MAGIC_SKIPPABLE_START|0xE)){
                            DEBUG("Last frame Header = %u does not match magic number %u. Ignoring malformed seektable.\n", skippableHeader, (ZSTD_MAGIC_SKIPPABLE_START|0xE));
                        }else{
                            const uint32_t _frameSize = load_le32(frame + 4);
                            if(_frameSize + ZSTD_SKIPPABLE_HEADER_SIZE != frameSize){
                                DEBUG("Last frame size = %u does not match expected size = %zu. Ignoring malformed seektable.\n", _frameSize + ZSTD_SKIPPABLE_HEADER_SIZE, frameSize);
                            }else{
                                uint8_t *table = frame + ZSTD_SKIPPABLE_HEADER_SIZE;
                                size_t cOffset = 0;
                                size_t dOffset = 0;
                                int32_t seektable_ok = 1;
                                for(uint32_t i = 0; i < numFrames; i++){
                                    if(addJumpTableRecord_(sctx->jt, cOffset, dOffset) != 0){
                                        seektable_ok = 0;
                                        break;
                                    }
                                    const uint32_t dc = load_le32(table + (i * sizePerEntry));
                                    const uint32_t dd = load_le32(table + (i * sizePerEntry) + 4);
                                    if(cOffset > SIZE_MAX - dc || dOffset > SIZE_MAX - dd){
                                        DEBUG("Seektable offset overflow at entry %u. Ignoring malformed seektable.\n", i);
                                        seektable_ok = 0;
                                        break;
                                    }
                                    cOffset += dc;
                                    dOffset += dd;
                                    if(cOffset > sctx->size){
                                        DEBUG("Seektable cOffset (%zu) exceeds buffer size (%zu). Ignoring malformed seektable.\n", cOffset, sctx->size);
                                        seektable_ok = 0;
                                        break;
                                    }
                                }
                                if(seektable_ok){
                                    if(addJumpTableRecord_(sctx->jt, cOffset, dOffset) != 0){
                                        seektable_ok = 0;
                                    }
                                }
                                if(seektable_ok){
                                    sctx->jumpTableFullyInitialized = 1;
                                    return 0;
                                }
                                /* Malformed seektable: clear partial records, fall through to frame scan */
                                sctx->jt->length = 0;
                                sctx->jumpTableFullyInitialized = 0;
                            }
                        }
                    }
                }
            }
        }
    } /* size >= ZSTD_SEEK_TABLE_FOOTER_SIZE */

    size_t frameCompressedSize;
    size_t compressedPos = 0;
    size_t uncompressedPos = 0;

    if(sctx->jt->length > 0){
        compressedPos = sctx->jt->records[sctx->jt->length-1].compressedPos;
        uncompressedPos = sctx->jt->records[sctx->jt->length-1].uncompressedPos;
    }

    buff = (uint8_t *)sctx->buff + compressedPos;
    if (compressedPos > size) {
        DEBUG("Compressed position exceeds buffer size\n");
        return -1;
    }
    size -= compressedPos; /* size now tracks remaining bytes from buff onwards */

    /* Probe DCtx and buffer for frames without content-size.
     * Allocated lazily on first need, reused across frames. */
    ZSTD_DCtx *probeDctx = NULL;
    void *probeBuff = NULL;
    size_t probeBuffSize = 0;
    int32_t result = -1;
    int32_t reachedTarget = 0;

    while (size > 0 && (frameCompressedSize = ZSTD_findFrameCompressedSize(buff, size))>0 && !ZSTD_isError(frameCompressedSize)) {
        const uint32_t magic = load_le32(buff);
        if((magic & ZSTD_MAGIC_SKIPPABLE_MASK) == ZSTD_MAGIC_SKIPPABLE_START){
            compressedPos += frameCompressedSize;
            buff = (uint8_t *)buff + frameCompressedSize;
            size -= frameCompressedSize;
            continue;
        }

        if(sctx->jt->length == 0 || sctx->jt->records[sctx->jt->length-1].uncompressedPos < uncompressedPos){
            if(addJumpTableRecord_(sctx->jt, compressedPos, uncompressedPos) != 0){
                goto cleanup;
            }
        }

        size_t frameContentSize = ZSTD_getFrameContentSize(buff, size);
        if(ZSTD_isError(frameContentSize)){//true if the uncompressed size is not known
            frameContentSize = 0;

            /* Lazy init: allocate probe resources on first unknown-size frame */
            if(!probeDctx){
                probeDctx = ZSTD_createDCtx();
                probeBuffSize = ZSTD_DStreamOutSize();
                probeBuff = malloc(probeBuffSize);
                if(!probeDctx || !probeBuff){
                    DEBUG("Unable to allocate probe resources\n");
                    goto cleanup;
                }
            }else{
                ZSTD_DCtx_reset(probeDctx, ZSTD_reset_session_only);
            }

            size_t lastRet = 0;
            ZSTD_inBuffer input = { buff, frameCompressedSize, 0 };
            while (input.pos < input.size) {
                ZSTD_outBuffer output = { probeBuff, probeBuffSize, 0 };
                lastRet = ZSTD_decompressStream(probeDctx, &output , &input);
                if(ZSTD_isError(lastRet)){
                    DEBUG("Error decompressing: %s\n", ZSTD_getErrorName(lastRet));
                    goto cleanup;
                }
                frameContentSize += output.pos;
            }

            if (lastRet != 0) {
                DEBUG("Unexpected EOF. Is the file truncated?\n");
                goto cleanup;
            }
        }

        compressedPos += frameCompressedSize;
        uncompressedPos += frameContentSize;
        buff = (uint8_t *)buff + frameCompressedSize;
        size -= frameCompressedSize;

        if(uncompressedPos >= upUntilPos){
            reachedTarget = 1;
            break;
        }
    }
    if(sctx->jt->length > 0){
        if(sctx->jt->records[sctx->jt->length-1].uncompressedPos < uncompressedPos){
            if(addJumpTableRecord_(sctx->jt, compressedPos, uncompressedPos) != 0){
                goto cleanup;
            }
        }
        if(!reachedTarget){
            sctx->jumpTableFullyInitialized = 1;
        }
        result = 0;
    }else{ //0 frames found
        DEBUG("No frames\n");
    }

cleanup:
    ZSTD_freeDCtx(probeDctx);
    free(probeBuff);
    return result;
}

bool ZSTDSeek_jumpTableIsInitialized(const ZSTDSeek_Context *sctx){
    return sctx->jumpTableFullyInitialized;
}

ZSTDSeek_JumpCoordinate ZSTDSeek_getJumpCoordinate(ZSTDSeek_Context *sctx, const size_t uncompressedPos) {
    if(!sctx->jumpTableFullyInitialized && (sctx->jt->length == 0 || sctx->jt->records[sctx->jt->length-1].uncompressedPos <= uncompressedPos)){
        if(ZSTDSeek_initializeJumpTableUpUntilPos(sctx, uncompressedPos) != 0){
            DEBUG("Jump table init failed for pos %zu, using partial table\n", uncompressedPos);
        }
    }

    //search for the greater value of m where sctx->jt->records[m].uncompressedPos <= uncompressedPos
    if(sctx->jt->length == 0){
        return (ZSTDSeek_JumpCoordinate){0, uncompressedPos, (ZSTDSeek_JumpTableRecord){0, 0}};
    }
    uint64_t l = 0;
    uint64_t r = sctx->jt->length - 1;
    while(l <= r){
        const uint64_t m = (l+r)/2;
        if(sctx->jt->records[m].uncompressedPos > uncompressedPos){
            if(m == 0) break;
            r = m-1;
        }else if((m+1) < sctx->jt->length && sctx->jt->records[m+1].uncompressedPos <= uncompressedPos){
            l = m+1;
        }else{
            return (ZSTDSeek_JumpCoordinate){sctx->jt->records[m].compressedPos, uncompressedPos - sctx->jt->records[m].uncompressedPos, sctx->jt->records[m]};
        }
    }
/*
    //old linear search
    for(uint64_t i = sctx->jt->length - 1; i >= 0; i--){
        if(sctx->jt->records[i].uncompressedPos <= uncompressedPos){
            return (ZSTDSeek_JumpCoordinate){sctx->jt->records[i].compressedPos, uncompressedPos - sctx->jt->records[i].uncompressedPos, sctx->jt->records[i]};
        }
    }
*/
    return (ZSTDSeek_JumpCoordinate){0, uncompressedPos, (ZSTDSeek_JumpTableRecord){0, 0}};
}

/* Seek API */

ZSTDSeek_Context* ZSTDSeek_createFromFileWithoutJumpTable(const char* file){
    const int32_t fd = open(file, O_RDONLY, 0);
    if(fd < 0){
        DEBUG("Unable to open '%s'\n", file);
        return NULL;
    }

    struct stat st;
    if(fstat(fd, &st) != 0 || st.st_size <= 0){
        DEBUG("Unable to stat or empty file '%s'\n", file);
        close(fd);
        return NULL;
    }

    void* const buff = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(buff == MAP_FAILED){
        DEBUG("Unable to mmap '%s'\n",  file);
        close(fd);
        return NULL;
    }

    ZSTDSeek_Context *sctx = ZSTDSeek_createWithoutJumpTable(buff, st.st_size);
    if(sctx){
        sctx->mmap_fd = fd;
        sctx->close_fd = 1;
        return sctx;
    }else{
        munmap(buff, st.st_size);
        close(fd);
        return NULL;
    }
}

ZSTDSeek_Context* ZSTDSeek_createFromFile(const char* file){
    ZSTDSeek_Context* sctx = ZSTDSeek_createFromFileWithoutJumpTable(file);
    if(sctx && ZSTDSeek_initializeJumpTable(sctx)!=0){
        DEBUG("Can't initialize the jump table\n");
        ZSTDSeek_free(sctx);
        return NULL;
    }
    return sctx;
}

ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptorWithoutJumpTable(const int32_t fd){
    struct stat st;
    if(fstat(fd, &st) != 0 || st.st_size <= 0){
        DEBUG("Unable to stat or empty file descriptor %d\n", fd);
        return NULL;
    }
    const size_t size = (size_t)st.st_size;

    void* const buff = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(buff == MAP_FAILED){
        DEBUG("Unable to mmap file descriptor %d\n",  fd);
        return NULL;
    }

    ZSTDSeek_Context *sctx = ZSTDSeek_createWithoutJumpTable(buff, size);
    if(sctx){
        sctx->mmap_fd = fd;
        sctx->close_fd = 0;
        return sctx;
    }else{
        munmap(buff, size);
        return NULL;
    }
}

ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptor(const int32_t fd){
    ZSTDSeek_Context* sctx = ZSTDSeek_createFromFileDescriptorWithoutJumpTable(fd);
    if(sctx && ZSTDSeek_initializeJumpTable(sctx)!=0){
        DEBUG("Can't initialize the jump table\n");
        ZSTDSeek_free(sctx);
        return NULL;
    }
    return sctx;
}

ZSTDSeek_Context* ZSTDSeek_createWithoutJumpTable(void *buff, const size_t size){
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if(!dctx){
        return NULL;
    }

    ZSTDSeek_Context* sctx = malloc(sizeof(ZSTDSeek_Context));
    if(!sctx){
        ZSTD_freeDCtx(dctx);
        return NULL;
    }

    sctx->dctx = dctx;

    sctx->buff = buff;
    sctx->size = size;

    sctx->inBuff = (uint8_t*)buff;

    sctx->currentUncompressedPos = 0;
    sctx->currentCompressedPos = 0;

    sctx->lastFrameCompressedSize = 0;

    sctx->jc = (ZSTDSeek_JumpCoordinate){0,0};

    sctx->tmpOutBuffSize = ZSTD_DStreamOutSize();
    sctx->tmpOutBuff = (uint8_t*)malloc(sctx->tmpOutBuffSize);
    if(!sctx->tmpOutBuff){
        ZSTD_freeDCtx(dctx);
        free(sctx);
        return NULL;
    }
    sctx->tmpOutBuffPos = 0;

    sctx->mmap_fd = -1;
    sctx->close_fd = 0;

    sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
    sctx->output = (ZSTD_outBuffer){sctx->tmpOutBuff, 0, 0};

    sctx->jt = ZSTDSeek_newJumpTable();
    if(!sctx->jt){
        ZSTD_freeDCtx(dctx);
        free(sctx->tmpOutBuff);
        free(sctx);
        return NULL;
    }
    sctx->jumpTableFullyInitialized = 0;

    //test if the buffer starts with a valid frame
    if(ZSTD_isError(ZSTD_findFrameCompressedSize(sctx->buff, sctx->size))){
        DEBUG("Invalid format\n");
        ZSTDSeek_free(sctx);
        return NULL;
    }

    return sctx;
}

ZSTDSeek_Context* ZSTDSeek_create(void *buff, const size_t size){
    ZSTDSeek_Context* sctx = ZSTDSeek_createWithoutJumpTable(buff, size);
    if(sctx && ZSTDSeek_initializeJumpTable(sctx)!=0){
        DEBUG("Can't initialize the jump table\n");
        ZSTDSeek_free(sctx);
        return NULL;
    }
    return sctx;
}

int64_t ZSTDSeek_read(void *outBuff, const size_t outBuffSize, ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return 0;
    }
    
    ZSTDSeek_getJumpCoordinate(sctx, sctx->currentUncompressedPos); //trigger the generation of a jump table record, if needed

    const size_t lastKnown = ZSTDSeek_lastKnownUncompressedFileSize(sctx);
    const size_t maxReadable = (lastKnown > sctx->currentUncompressedPos) ? lastKnown - sctx->currentUncompressedPos : 0;
    size_t toRead = maxReadable < outBuffSize ? maxReadable : outBuffSize;
    const size_t shouldRead = toRead;

    if(sctx->tmpOutBuffPos < sctx->output.pos){
        const size_t available = sctx->output.pos - sctx->tmpOutBuffPos;
        if(sctx->jc.uncompressedOffset >= available){
            sctx->jc.uncompressedOffset -= available;
            sctx->tmpOutBuffPos = sctx->output.pos;
        }else{
            const size_t maxCopy = available - sctx->jc.uncompressedOffset;
            size_t toCopy = maxCopy < toRead ? maxCopy : toRead;

            memcpy(outBuff, sctx->tmpOutBuff+sctx->tmpOutBuffPos+sctx->jc.uncompressedOffset, toCopy);
            toRead -= toCopy;
            outBuff = (uint8_t *)outBuff + toCopy;
            sctx->currentUncompressedPos += toCopy;
            sctx->tmpOutBuffPos += toCopy + sctx->jc.uncompressedOffset;
            sctx->jc.uncompressedOffset = 0;
        }
    }

    while(toRead > 0){
        if(sctx->input.pos >= sctx->input.size){
            /* Compute remaining bytes from inBuff to end of buffer */
            const size_t consumed = (size_t)((uint8_t *)sctx->inBuff - (uint8_t *)sctx->buff);
            const size_t remaining = (consumed < sctx->size) ? sctx->size - consumed : 0;
            if(remaining == 0){
                break;
            }
            sctx->lastFrameCompressedSize = ZSTD_findFrameCompressedSize(sctx->inBuff, remaining);
            if(ZSTD_isError(sctx->lastFrameCompressedSize) || sctx->lastFrameCompressedSize == 0){
                break;
            }

            /* Skip skippable frames (e.g. seekable format footer) without
             * passing them to the decompressor. */
            const uint32_t magic = load_le32(sctx->inBuff);
            if((magic & ZSTD_MAGIC_SKIPPABLE_MASK) == ZSTD_MAGIC_SKIPPABLE_START){
                sctx->inBuff += sctx->lastFrameCompressedSize;
                sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
                continue;
            }

            sctx->input = (ZSTD_inBuffer){sctx->inBuff, sctx->lastFrameCompressedSize, 0};
        }

        while(sctx->input.pos < sctx->input.size){
            sctx->output = (ZSTD_outBuffer){ sctx->tmpOutBuff, sctx->tmpOutBuffSize, 0 };
            sctx->tmpOutBuffPos = 0;
            const size_t ret = ZSTD_decompressStream(sctx->dctx, &sctx->output , &sctx->input);

            if(ZSTD_isError(ret)){
                DEBUG("Error decompressing: %s\n", ZSTD_getErrorName(ret));
                sctx->currentCompressedPos =
                    (size_t)((uint8_t *)sctx->inBuff - (uint8_t *)sctx->buff) + sctx->input.pos;
                return ZSTDSEEK_ERR_READ;
            }

            {
                const size_t available = sctx->output.pos - sctx->tmpOutBuffPos;
                if(sctx->jc.uncompressedOffset >= available){
                    sctx->jc.uncompressedOffset -= available;
                    sctx->tmpOutBuffPos = sctx->output.pos;
                }else{
                    const size_t maxCopy = available - sctx->jc.uncompressedOffset;
                    size_t toCopy = maxCopy < toRead ? maxCopy : toRead;

                    memcpy(outBuff, sctx->tmpOutBuff+sctx->tmpOutBuffPos+sctx->jc.uncompressedOffset, toCopy);
                    toRead -= toCopy;
                    outBuff = (uint8_t *)outBuff + toCopy;
                    sctx->currentUncompressedPos += toCopy;
                    sctx->tmpOutBuffPos += toCopy + sctx->jc.uncompressedOffset;
                    sctx->jc.uncompressedOffset = 0;
                }
            }

            if(toRead == 0){
                break;
            }
        }

        if(sctx->input.pos == sctx->input.size){ //end of frame
            sctx->inBuff+=sctx->lastFrameCompressedSize;
            sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
        }

        if(toRead == 0){
            break;
        }
    }

    /* Compute currentCompressedPos from absolute pointers so it stays
     * consistent regardless of which code path was taken above. */
    sctx->currentCompressedPos =
        (size_t)((uint8_t *)sctx->inBuff - (uint8_t *)sctx->buff) + sctx->input.pos;

    return shouldRead - toRead;
}

int32_t ZSTDSeek_seek(ZSTDSeek_Context *sctx, int64_t offset, int32_t origin){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }
    if(origin == SEEK_CUR){
        if(offset==0){
            return 0;
        }
        offset = (int64_t)sctx->currentUncompressedPos + offset;
        origin = SEEK_SET;
    }else if(origin == SEEK_END){
        offset = (int64_t)ZSTDSeek_uncompressedFileSize(sctx) + offset;
        origin = SEEK_SET;
    }
    if(origin == SEEK_SET){
        if(offset < 0){
            DEBUG("Negative seek\n");
            return ZSTDSEEK_ERR_NEGATIVE_SEEK;
        }else if(offset > 0){
            ZSTDSeek_getJumpCoordinate(sctx, (size_t)offset); //trigger an update of the lastKnownUncompressedFileSize
            if((size_t)offset > ZSTDSeek_lastKnownUncompressedFileSize(sctx)){
                DEBUG("Seek to a frame beyond the buffer length\n");
                return ZSTDSEEK_ERR_BEYOND_END_SEEK;
            }
        }

        if(offset == sctx->currentUncompressedPos){ //we are already there, do nothing
            return 0;
        }

        const ZSTDSeek_JumpCoordinate new_jc = ZSTDSeek_getJumpCoordinate(sctx, (size_t)offset);

        if(sctx->jc.compressedOffset != new_jc.compressedOffset || offset < sctx->currentUncompressedPos){ //reset
            ZSTD_DCtx_reset(sctx->dctx, ZSTD_reset_session_only);

            sctx->jc = new_jc;

            sctx->inBuff = (uint8_t *)sctx->buff + sctx->jc.compressedOffset; //jump to the beginning of the frame..
            sctx->currentUncompressedPos = offset; //..and adjust the uncompressed position..
            sctx->currentCompressedPos = sctx->jc.compressedOffset;
            sctx->tmpOutBuffPos = 0; //..and reset the position in the tmp buffer
            sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
            sctx->output = (ZSTD_outBuffer){sctx->tmpOutBuff, 0, 0};

        }else{ //move forward
            size_t toSkipTotal = offset - sctx->currentUncompressedPos;

            /* Stack-allocated discard buffer. ZSTDSeek_read decompresses
             * internally into sctx->tmpOutBuff (128 KB) and copies to this
             * buffer, so a small size only adds loop iterations for the
             * copy step — decompression throughput is unaffected. */
            uint8_t discardBuf[4096];

            while(toSkipTotal>0){
                const size_t toSkip = sizeof(discardBuf) < toSkipTotal ? sizeof(discardBuf) : toSkipTotal;
                const int64_t bytesRead = ZSTDSeek_read(discardBuf, toSkip, sctx);
                if(bytesRead < 0){
                    return ZSTDSEEK_ERR_READ; /* decompression error */
                }
                if(bytesRead == 0){
                    break; /* no progress */
                }
                toSkipTotal -= (size_t)bytesRead;
            }
            if(toSkipTotal > 0){
                return ZSTDSEEK_ERR_READ;
            }
        }
    }else{
        DEBUG("Invalid origin\n");
        return -1;
    }

    return 0;
}

int64_t ZSTDSeek_tell(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }

    return (int64_t)sctx->currentUncompressedPos;
}

int64_t ZSTDSeek_compressedTell(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }

    return (int64_t)sctx->currentCompressedPos;
}

size_t ZSTDSeek_uncompressedFileSize(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return 0;
    }

    ZSTDSeek_initializeJumpTable(sctx);

    return ZSTDSeek_lastKnownUncompressedFileSize(sctx);
}

size_t ZSTDSeek_lastKnownUncompressedFileSize(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return 0;
    }

    return sctx->jt->length > 0 ? sctx->jt->records[sctx->jt->length-1].uncompressedPos : 0;
}

int32_t ZSTDSeek_fileno(const ZSTDSeek_Context *sctx){
    return sctx->mmap_fd;
}

size_t ZSTDSeek_countFramesUpTo(ZSTDSeek_Context *sctx, const size_t upTo){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return 0;
    }

    size_t frameCompressedSize;

    void *buff = sctx->buff;
    size_t remaining = sctx->size;

    size_t counter = 0;

    while (remaining > 0 && (frameCompressedSize = ZSTD_findFrameCompressedSize(buff, remaining))>0 && !ZSTD_isError(frameCompressedSize)) {
        counter++;
        buff = (uint8_t *)buff + frameCompressedSize;
        remaining -= frameCompressedSize;
        if(counter >= upTo){
            return upTo;
        }
    }

    return counter;
}

size_t ZSTDSeek_getNumberOfFrames(ZSTDSeek_Context *sctx){
    return ZSTDSeek_countFramesUpTo(sctx, SIZE_MAX);
}

bool ZSTDSeek_isMultiframe(ZSTDSeek_Context *sctx){
    return ZSTDSeek_countFramesUpTo(sctx, 2) > 1;
}

void ZSTDSeek_free(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return;
    }

    if(sctx->dctx){
        ZSTD_freeDCtx(sctx->dctx);
    }

    ZSTDSeek_freeJumpTable(sctx->jt);

    if(sctx->mmap_fd >= 0){
        munmap(sctx->buff, sctx->size);
        if(sctx->close_fd){
            close(sctx->mmap_fd);
        }
    }

    free(sctx->tmpOutBuff);

    free(sctx);
}
