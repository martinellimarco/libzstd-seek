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
#include <malloc.h>
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

    size_t currentUncompressedPos; //the current position in the uncompressed file, returned by tell

    ZSTDSeek_JumpTable* jt;

    ZSTDSeek_JumpCoordinate jc;

    size_t tmpOutBuffSize;
    uint8_t* tmpOutBuff;
    size_t tmpOutBuffPos; //the position where we read so far in the tmpOutBuff. if tmpOutBuffPos < output.pos we have data left in this buffer to read before we move on to uncompress more data

    int mmap_fd; //the file descriptor of the memory map, used only if the context is created with ZSTDSeek_createFromFile

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

void ZSTDSeek_addJumpTableRecord(ZSTDSeek_JumpTable* jt, uint32_t frameIdx, size_t compressedPos, size_t uncompressedPos){
    if(!jt){
        DEBUG("Invalid argument");
        return;
    }

    if(jt->length == jt->capacity){
        jt->capacity *= 2;
        jt->records = realloc(jt->records, jt->capacity*sizeof(ZSTDSeek_JumpTableRecord));
    }

    jt->records[jt->length++] = (ZSTDSeek_JumpTableRecord){
        frameIdx,
        compressedPos,
        uncompressedPos
    };
}

int ZSTDSeek_initializeJumpTable(ZSTDSeek_Context *sctx, void *buff, size_t size){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return -1;
    }
    /*
     * TODO check if the last frame in buff is a skippable frame (magic number 0x184D2A5E).
     * If so check if it ends with the seekable format magic number 0x8F92EAB1.
     * If so import the seekable format jump table instead of doing the work again.
    */

    int frameCompressedSize;
    size_t compressedPos = 0;
    size_t uncompressedPos = 0;
    uint32_t frameIdx=0;

    while ((frameCompressedSize = ZSTD_findFrameCompressedSize(buff, size))>0) {
        size_t frameContentSize = ZSTD_getFrameContentSize(buff, size);

        ZSTDSeek_addJumpTableRecord(sctx->jt, frameIdx, compressedPos, uncompressedPos);

        if(ZSTD_isError(frameContentSize)){//true if the uncompressed size is not known
            frameContentSize = 0;

            size_t const buffOutSize = ZSTD_DStreamOutSize();
            void*  const buffOut = malloc(buffOutSize);
            size_t lastRet = 0;
            void *buffIn = buff;

            ZSTD_inBuffer input = { buffIn, frameCompressedSize, 0 };
            while (input.pos < input.size) {
                ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
                lastRet = ZSTD_decompressStream(sctx->dctx, &output , &input);
                if(ZSTD_isError(lastRet)){
                    DEBUG("Error decompressing: %s\n", ZSTD_getErrorName(lastRet));
                    free(buffOut);
                    return -1;
                }
                frameContentSize += output.pos;
            }

            free(buffOut);

            if (lastRet != 0) {
                DEBUG("Unexpected EOF\n");
                return -1;
            }
        }

        frameIdx++;
        compressedPos += frameCompressedSize;
        uncompressedPos += frameContentSize;
        buff += frameCompressedSize;
    }
    if(frameIdx>0){
        sctx->jt->uncompressedFileSize = uncompressedPos;
        return 0;
    }else{ //0 frames found
        DEBUG("No frames\n");
        return -1;
    }
}

ZSTDSeek_JumpCoordinate ZSTDSeek_getJumpCoordinate(ZSTDSeek_Context *sctx, size_t uncompressedPos) {
    for(uint32_t i = sctx->jt->length - 1; i >= 0; i--){
        if(sctx->jt->records[i].uncompressedPos <= uncompressedPos){
            return (ZSTDSeek_JumpCoordinate){sctx->jt->records[i].compressedPos, uncompressedPos - sctx->jt->records[i].uncompressedPos, sctx->jt->records[i]};
        }
    }
    return (ZSTDSeek_JumpCoordinate){0, uncompressedPos, (ZSTDSeek_JumpTableRecord){0, 0, 0}};
}

/* Seek API */

ZSTDSeek_Context* ZSTDSeek_createFromFile(const char* file){
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

    ZSTDSeek_Context *sctx = ZSTDSeek_create(buff, st.st_size);
    if(sctx){
        sctx->mmap_fd = fd;
        return sctx;
    }else{
        munmap(buff, st.st_size);
        close(fd);
        return NULL;
    }
}

ZSTDSeek_Context* ZSTDSeek_create(void *buff, size_t size){
    ZSTD_DCtx *dctx = ZSTD_createDCtx();

    ZSTDSeek_Context* sctx = malloc(sizeof(ZSTDSeek_Context));

    sctx->dctx = dctx;

    sctx->buff = buff;
    sctx->size = size;

    sctx->inBuff = (uint8_t*)buff;

    sctx->currentUncompressedPos = 0;

    sctx->jc = (ZSTDSeek_JumpCoordinate){0,0};

    sctx->tmpOutBuffSize = ZSTD_DStreamOutSize();
    sctx->tmpOutBuff = (uint8_t*)malloc(sctx->tmpOutBuffSize);
    sctx->tmpOutBuffPos = 0;

    sctx->mmap_fd = 0;

    sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
    sctx->output = (ZSTD_outBuffer){sctx->tmpOutBuff, 0, 0};

    sctx->jt = ZSTDSeek_newJumpTable();
    if(ZSTDSeek_initializeJumpTable(sctx, buff, size)!=0){
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

    size_t maxReadable = sctx->jt->uncompressedFileSize - sctx->currentUncompressedPos;
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

    int frameCompressedSize;
    while (toRead > 0 && ((sctx->input.pos < sctx->input.size) || (frameCompressedSize = ZSTD_findFrameCompressedSize(sctx->inBuff, sctx->size)) > 0)){
        if(sctx->input.pos == sctx->input.size){
            sctx->input = (ZSTD_inBuffer){sctx->inBuff, (size_t)frameCompressedSize, 0};
        }

        while (sctx->input.pos < sctx->input.size) {
            sctx->output = (ZSTD_outBuffer){ sctx->tmpOutBuff, sctx->tmpOutBuffSize, 0 };
            sctx->tmpOutBuffPos = 0;
            size_t const ret = ZSTD_decompressStream(sctx->dctx, &sctx->output , &sctx->input);

            if(ZSTD_isError(ret)){
                DEBUG("Error decompressing: %s\n", ZSTD_getErrorName(ret));
                return ZSTDSEEK_ERR_READ;
            }

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

        sctx->inBuff+=frameCompressedSize;

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
        offset = (long)sctx->currentUncompressedPos + offset;
        origin = SEEK_SET;
    }else if(origin == SEEK_END){
        offset = (long)sctx->jt->uncompressedFileSize + offset;
        origin = SEEK_SET;
    }
    if(origin == SEEK_SET){
        if(offset < 0){
            DEBUG("Negative seek\n");
            return ZSTDSEEK_ERR_NEGATIVE_SEEK;
        }else if(sctx->jt->uncompressedFileSize>0 && offset > sctx->jt->uncompressedFileSize){
            DEBUG("Seek to a frame beyond the buffer length\n");
            return ZSTDSEEK_ERR_BEYOND_END_SEEK;
        }

        if(offset == sctx->currentUncompressedPos){ //we are already there, do nothing
            return 0;
        }

        ZSTDSeek_JumpCoordinate new_jc = ZSTDSeek_getJumpCoordinate(sctx, offset);

        ZSTD_DCtx_reset(sctx->dctx, ZSTD_reset_session_only);

        sctx->jc = new_jc;

        sctx->inBuff = sctx->buff + sctx->jc.compressedOffset; //jump to the beginning of the frame..
        sctx->currentUncompressedPos = offset; //..and adjust the uncompressed position..
        sctx->tmpOutBuffPos = 0; //..and reset the position in the tmp buffer
        sctx->input = (ZSTD_inBuffer){sctx->inBuff, 0, 0};
        sctx->output = (ZSTD_outBuffer){sctx->tmpOutBuff, 0, 0};
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

void ZSTDSeek_free(ZSTDSeek_Context *sctx){
    if(!sctx){
        DEBUG("ZSTDSeek_Context is NULL\n");
        return;
    }

    if(sctx->dctx){
        ZSTD_freeDCtx(sctx->dctx);
    }

    ZSTDSeek_freeJumpTable(sctx->jt);

    if(sctx->mmap_fd){
        munmap(sctx->buff, sctx->size);
        close(sctx->mmap_fd);
    }

    free(sctx->tmpOutBuff);

    free(sctx);
}
