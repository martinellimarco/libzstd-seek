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

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include "zstd-seek.h"

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
    int jumpTableFullyInitialized;

    ZSTDSeek_JumpCoordinate jc;

    size_t tmpOutBuffSize;
    uint8_t* tmpOutBuff;
    size_t tmpOutBuffPos; //the position where we read so far in the tmpOutBuff. if tmpOutBuffPos < output.pos we have data left in this buffer to read before we move on to uncompress more data

    int mmap_fd; //the file descriptor of the memory map, used only if the context is created with ZSTDSeek_createFromFile or ZSTDSeek_createFromFileWithoutJumpTable
    int close_fd; //1 if we own the mmap_fd and we are responsible of closing it, 0 otherwise

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

    jt->records = malloc(sizeof(ZSTDSeek_JumpTableRecord));
    jt->length = 0;
    jt->capacity = 1;

    return jt;
}

void ZSTDSeek_freeJumpTable(ZSTDSeek_JumpTable* jt){
    if(!jt){
        DEBUG("Invalid argument");
        return;
    }
    free(jt->records);
    free(jt);
}

void ZSTDSeek_addJumpTableRecord(ZSTDSeek_JumpTable* jt, size_t compressedPos, size_t uncompressedPos){
    if(!jt){
        DEBUG("Invalid argument");
        return;
    }

    if(jt->length == jt->capacity){
        jt->capacity *= 2;
        jt->records = realloc(jt->records, jt->capacity*sizeof(ZSTDSeek_JumpTableRecord));
    }

    jt->records[jt->length++] = (ZSTDSeek_JumpTableRecord){
            compressedPos,
            uncompressedPos
    };
}

int ZSTDSeek_initializeJumpTable(ZSTDSeek_Context *sctx){
    return ZSTDSeek_initializeJumpTableUpUntilPos(sctx, SIZE_MAX);
}

int ZSTDSeek_initializeJumpTableUpUntilPos(ZSTDSeek_Context *sctx, size_t upUntilPos){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }

    void *buff = sctx->buff;
    size_t size = sctx->size;

    void *footer = buff + (size - ZSTD_SEEK_TABLE_FOOTER_SIZE);
    uint32_t magicnumber = *((uint32_t *)(footer + 5));

    if(magicnumber == ZSTD_SEEKABLE_MAGICNUMBER){
        DEBUG("Seektable detected\n");
        uint8_t sfd = *((uint8_t*)(footer + 4));
        uint8_t checksumFlag = sfd >> 7;

        /* check reserved bits */
        if((sfd >> 2) & 0x1f){
            DEBUG("Last frame checksumFlag= %x: Bits 3-7 should be zero. Ignoring malformed seektable.\n",(uint32_t)sfd);
        }else{
            uint32_t const numFrames = *((uint32_t *)footer);
            uint32_t const sizePerEntry = 8 + (checksumFlag ? 4 : 0);
            uint32_t const tableSize = sizePerEntry * numFrames;
            uint32_t const frameSize = tableSize + ZSTD_SEEK_TABLE_FOOTER_SIZE + ZSTD_SKIPPABLE_HEADER_SIZE;

            void *frame = buff + (size - frameSize);
            uint32_t skippableHeader = *((uint32_t *)frame);
            if(skippableHeader != (ZSTD_MAGIC_SKIPPABLE_START|0xE)){
                DEBUG("Last frame Header = %u does not match magic number %u. Ignoring malformed seektable.\n", skippableHeader, (ZSTD_MAGIC_SKIPPABLE_START|0xE));
            }else{
                uint32_t _frameSize = *((uint32_t *)(frame + 4));
                if(_frameSize + ZSTD_SKIPPABLE_HEADER_SIZE != frameSize){
                    DEBUG("Last frame size = %u does not match expected size = %u. Ignoring malformed seektable.\n", _frameSize + ZSTD_SKIPPABLE_HEADER_SIZE, frameSize);
                }else{
                    void *table = frame + ZSTD_SKIPPABLE_HEADER_SIZE;
                    uint32_t cOffset = 0;
                    uint32_t dOffset = 0;
                    for(uint32_t i = 0; i < numFrames; i++){
                        ZSTDSeek_addJumpTableRecord(sctx->jt, cOffset, dOffset);
                        cOffset += *((uint32_t *)(table + (i * sizePerEntry)));
                        dOffset += *((uint32_t *)(table + (i * sizePerEntry) + 4));
                    }
                    ZSTDSeek_addJumpTableRecord(sctx->jt, cOffset, dOffset);

                    sctx->jumpTableFullyInitialized = 1;
                    return 0;
                }
            }
        }
    }

    size_t frameCompressedSize;
    size_t compressedPos = 0;
    size_t uncompressedPos = 0;

    if(sctx->jt->length > 0){
        compressedPos = sctx->jt->records[sctx->jt->length-1].compressedPos;
        uncompressedPos = sctx->jt->records[sctx->jt->length-1].uncompressedPos;
    }

    buff = sctx->buff + compressedPos;

    sctx->jumpTableFullyInitialized = 1;

    while ((frameCompressedSize = ZSTD_findFrameCompressedSize(buff, size))>0 && !ZSTD_isError(frameCompressedSize)) {
        size_t frameContentSize = ZSTD_getFrameContentSize(buff, size);

        if(sctx->jt->length == 0 || sctx->jt->records[sctx->jt->length-1].uncompressedPos < uncompressedPos){
            ZSTDSeek_addJumpTableRecord(sctx->jt, compressedPos, uncompressedPos);
        }

        if(ZSTD_isError(frameContentSize)){//true if the uncompressed size is not known
            frameContentSize = 0;

            ZSTD_DCtx *dctx = ZSTD_createDCtx();
            size_t const buffOutSize = ZSTD_DStreamOutSize();
            void*  const buffOut = malloc(buffOutSize);
            size_t lastRet = 0;
            void *buffIn = buff;

            ZSTD_inBuffer input = { buffIn, frameCompressedSize, 0 };
            while (input.pos < input.size) {
                ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
                lastRet = ZSTD_decompressStream(dctx, &output , &input);
                if(ZSTD_isError(lastRet)){
                    DEBUG("Error decompressing: %s\n", ZSTD_getErrorName(lastRet));
                    ZSTD_freeDCtx(dctx);
                    free(buffOut);
                    return -1;
                }
                frameContentSize += output.pos;
            }
            ZSTD_freeDCtx(dctx);
            free(buffOut);

            if (lastRet != 0) {
                DEBUG("Unexpected EOF. Is the file truncated?\n");
                return -1;
            }
        }

        compressedPos += frameCompressedSize;
        uncompressedPos += frameContentSize;
        buff += frameCompressedSize;

        if(uncompressedPos >= upUntilPos){
            sctx->jumpTableFullyInitialized = 0;
            break;
        }
    }
    if(sctx->jt->length > 0){
        if(sctx->jt->records[sctx->jt->length-1].uncompressedPos < uncompressedPos){
            ZSTDSeek_addJumpTableRecord(sctx->jt, compressedPos, uncompressedPos);
        }
        return 0;
    }else{ //0 frames found
        DEBUG("No frames\n");
        return -1;
    }
}

int ZSTDSeek_jumpTableIsInitialized(ZSTDSeek_Context *sctx){
    return sctx->jumpTableFullyInitialized;
}

ZSTDSeek_JumpCoordinate ZSTDSeek_getJumpCoordinate(ZSTDSeek_Context *sctx, size_t uncompressedPos) {
    if(!sctx->jumpTableFullyInitialized && (sctx->jt->length == 0 || sctx->jt->records[sctx->jt->length-1].uncompressedPos <= uncompressedPos)){
        ZSTDSeek_initializeJumpTableUpUntilPos(sctx, uncompressedPos);
    }

    //search for the greater value of m where sctx->jt->records[m].uncompressedPos <= uncompressedPos
    uint32_t l = 0;
    uint32_t r = sctx->jt->length - 1;
    while(l <= r){
        uint32_t m = (l+r)/2;
        if(sctx->jt->records[m].uncompressedPos > uncompressedPos){
            r = m-1;
        }else if((m+1) < sctx->jt->length && sctx->jt->records[m+1].uncompressedPos <= uncompressedPos){
            l = m+1;
        }else{
            return (ZSTDSeek_JumpCoordinate){sctx->jt->records[m].compressedPos, uncompressedPos - sctx->jt->records[m].uncompressedPos, sctx->jt->records[m]};
        }
    }
/*
    //old linear search
    for(uint32_t i = sctx->jt->length - 1; i >= 0; i--){
        if(sctx->jt->records[i].uncompressedPos <= uncompressedPos){
            return (ZSTDSeek_JumpCoordinate){sctx->jt->records[i].compressedPos, uncompressedPos - sctx->jt->records[i].uncompressedPos, sctx->jt->records[i]};
        }
    }
*/
    return (ZSTDSeek_JumpCoordinate){0, uncompressedPos, (ZSTDSeek_JumpTableRecord){0, 0}};
}

/* Seek API */

ZSTDSeek_Context* ZSTDSeek_createFromFileWithoutJumpTable(const char* file){
    struct stat st;
    stat(file, &st);

    int fd = open(file, O_RDONLY, 0);
    if(fd < 0){
        DEBUG("Unable to open '%s'\n", file);
        return NULL;
    }

    void* const buff = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(buff == MAP_FAILED){
        DEBUG("Unable to mmap '%s'\n",  file);
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

ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptorWithoutJumpTable(int fd){
    size_t size = lseek(fd,0L,SEEK_END);

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
        close(fd);
        return NULL;
    }
}

ZSTDSeek_Context* ZSTDSeek_createFromFileDescriptor(int fd){
    ZSTDSeek_Context* sctx = ZSTDSeek_createFromFileDescriptorWithoutJumpTable(fd);
    if(sctx && ZSTDSeek_initializeJumpTable(sctx)!=0){
        DEBUG("Can't initialize the jump table\n");
        ZSTDSeek_free(sctx);
        return NULL;
    }
    return sctx;
}

ZSTDSeek_Context* ZSTDSeek_createWithoutJumpTable(void *buff, size_t size){
    ZSTD_DCtx *dctx = ZSTD_createDCtx();

    ZSTDSeek_Context* sctx = malloc(sizeof(ZSTDSeek_Context));

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
    sctx->tmpOutBuffPos = 0;

    sctx->mmap_fd = -1;
    sctx->close_fd = 0;

    sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
    sctx->output = (ZSTD_outBuffer){sctx->tmpOutBuff, 0, 0};

    sctx->jt = ZSTDSeek_newJumpTable();
    sctx->jumpTableFullyInitialized = 0;

    //test if the buffer starts with a valid frame
    if(ZSTD_isError(ZSTD_findFrameCompressedSize(sctx->buff, sctx->size))){
        DEBUG("Invalid format\n");
        ZSTDSeek_free(sctx);
        return NULL;
    }

    return sctx;
}

ZSTDSeek_Context* ZSTDSeek_create(void *buff, size_t size){
    ZSTDSeek_Context* sctx = ZSTDSeek_createWithoutJumpTable(buff, size);
    if(sctx && ZSTDSeek_initializeJumpTable(sctx)!=0){
        DEBUG("Can't initialize the jump table\n");
        ZSTDSeek_free(sctx);
        return NULL;
    }
    return sctx;
}

size_t ZSTDSeek_read(void *outBuff, size_t outBuffSize, ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return 0;
    }
    
    ZSTDSeek_JumpCoordinate localJc = ZSTDSeek_getJumpCoordinate(sctx, sctx->currentUncompressedPos); //trigger the generation of a jump table record, if needed
    sctx->currentCompressedPos = localJc.jtr.compressedPos;

    size_t maxReadable = ZSTDSeek_lastKnownUncompressedFileSize(sctx) - sctx->currentUncompressedPos;
    size_t toRead = maxReadable < outBuffSize ? maxReadable : outBuffSize;
    size_t shouldRead = toRead;

    if(sctx->tmpOutBuffPos < sctx->output.pos){
        if(sctx->jc.uncompressedOffset > sctx->output.pos){
            sctx->jc.uncompressedOffset -= sctx->output.pos;
        }else{
            size_t maxCopy = (sctx->output.pos - sctx->tmpOutBuffPos) - sctx->jc.uncompressedOffset;
            size_t toCopy = maxCopy < toRead ? maxCopy : toRead;

            memcpy(outBuff, sctx->tmpOutBuff+sctx->tmpOutBuffPos+sctx->jc.uncompressedOffset, toCopy);
            toRead -= toCopy;
            outBuff += toCopy;
            sctx->currentUncompressedPos += toCopy;
            sctx->tmpOutBuffPos += toCopy + sctx->jc.uncompressedOffset;
            sctx->jc.uncompressedOffset = 0;
        }
    }

    while (toRead > 0 && ((sctx->input.pos < sctx->input.size) || (sctx->lastFrameCompressedSize = ZSTD_findFrameCompressedSize(sctx->inBuff, sctx->size)) > 0)){
        if(sctx->input.pos == sctx->input.size){
            sctx->input = (ZSTD_inBuffer){sctx->inBuff, sctx->lastFrameCompressedSize, 0};
        }

        while (sctx->input.pos < sctx->input.size) {
            sctx->output = (ZSTD_outBuffer){ sctx->tmpOutBuff, sctx->tmpOutBuffSize, 0 };
            sctx->tmpOutBuffPos = 0;
            size_t const ret = ZSTD_decompressStream(sctx->dctx, &sctx->output , &sctx->input);

            if(ZSTD_isError(ret)){
                DEBUG("Error decompressing: %s\n", ZSTD_getErrorName(ret));
                return ZSTDSEEK_ERR_READ;
            }

            sctx->currentCompressedPos += sctx->input.pos;

            if(sctx->jc.uncompressedOffset > sctx->output.pos){
                sctx->jc.uncompressedOffset -= sctx->output.pos;
            }else{
                size_t maxCopy = (sctx->output.pos - sctx->tmpOutBuffPos) - sctx->jc.uncompressedOffset;
                size_t toCopy = maxCopy < toRead ? maxCopy : toRead;

                memcpy(outBuff, sctx->tmpOutBuff+sctx->tmpOutBuffPos+sctx->jc.uncompressedOffset, toCopy);
                toRead -= toCopy;
                outBuff += toCopy;
                sctx->currentUncompressedPos += toCopy;
                sctx->tmpOutBuffPos += toCopy + sctx->jc.uncompressedOffset;
                sctx->jc.uncompressedOffset = 0;
            }

            if(toRead == 0){
                break;
            }
        }

        if(sctx->input.pos == sctx->input.size){ //end of frame
            sctx->inBuff+=sctx->lastFrameCompressedSize;
        }

        if(toRead == 0){
            break;
        }
    }

    return shouldRead - toRead;
}

int ZSTDSeek_seek(ZSTDSeek_Context *sctx, long offset, int origin){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }
    if(origin == SEEK_CUR){
        if(offset==0){
            return 0;
        }
        offset = (long)sctx->currentUncompressedPos + offset;
        origin = SEEK_SET;
    }else if(origin == SEEK_END){
        offset = (long)ZSTDSeek_uncompressedFileSize(sctx) + offset;
        origin = SEEK_SET;
    }
    if(origin == SEEK_SET){
        if(offset < 0){
            DEBUG("Negative seek\n");
            return ZSTDSEEK_ERR_NEGATIVE_SEEK;
        }else if(offset > 0){
            ZSTDSeek_getJumpCoordinate(sctx, sctx->currentUncompressedPos+offset); //trigger an update of the lastKnownUncompressedFileSize
            if(offset > ZSTDSeek_lastKnownUncompressedFileSize(sctx)){
                DEBUG("Seek to a frame beyond the buffer length\n");
                return ZSTDSEEK_ERR_BEYOND_END_SEEK;
            }
        }

        if(offset == sctx->currentUncompressedPos){ //we are already there, do nothing
            return 0;
        }

        ZSTDSeek_JumpCoordinate new_jc = ZSTDSeek_getJumpCoordinate(sctx, offset);

        if(sctx->jc.compressedOffset != new_jc.compressedOffset || offset < sctx->currentUncompressedPos){ //reset
            ZSTD_DCtx_reset(sctx->dctx, ZSTD_reset_session_only);

            sctx->jc = new_jc;

            sctx->inBuff = sctx->buff + sctx->jc.compressedOffset; //jump to the beginning of the frame..
            sctx->currentUncompressedPos = offset; //..and adjust the uncompressed position..
            sctx->currentCompressedPos = sctx->jc.compressedOffset;
            sctx->tmpOutBuffPos = 0; //..and reset the position in the tmp buffer
            sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
            sctx->output = (ZSTD_outBuffer){sctx->tmpOutBuff, 0, 0};

        }else{ //move forward
            size_t toSkipTotal = offset - sctx->currentUncompressedPos;

            size_t const buffOutSize = ZSTD_DStreamOutSize();
            void*  const buffOut = malloc(buffOutSize);

            while(toSkipTotal>0){
                size_t toSkip = buffOutSize < toSkipTotal ? buffOutSize : toSkipTotal;
                toSkipTotal -= ZSTDSeek_read(buffOut, toSkip, sctx);
            }

            free(buffOut);
        }
    }else{
        DEBUG("Invalid origin\n");
        return -1;
    }

    return 0;
}

long ZSTDSeek_tell(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }

    return sctx->currentUncompressedPos;
}

long ZSTDSeek_compressedTell(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }

    return sctx->currentCompressedPos;
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

int ZSTDSeek_fileno(ZSTDSeek_Context *sctx){
    return sctx->mmap_fd;
}

size_t ZSTDSeek_countFramesUpTo(ZSTDSeek_Context *sctx, size_t upTo){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return 0;
    }

    size_t frameCompressedSize;

    void *buff = sctx->buff;
    size_t size = sctx->size;

    size_t counter = 0;

    while ((frameCompressedSize = ZSTD_findFrameCompressedSize(buff, size))>0 && !ZSTD_isError(frameCompressedSize)) {
        counter++;
        buff += frameCompressedSize;
        if(counter >= upTo){
            return upTo;
        }
    }

    return counter;
}

size_t ZSTDSeek_getNumberOfFrames(ZSTDSeek_Context *sctx){
    return ZSTDSeek_countFramesUpTo(sctx, SIZE_MAX);
}

int ZSTDSeek_isMultiframe(ZSTDSeek_Context *sctx){
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

    if(sctx->mmap_fd>=0 && sctx->close_fd){
        munmap(sctx->buff, sctx->size);
        close(sctx->mmap_fd);
    }

    free(sctx->tmpOutBuff);

    free(sctx);
}
