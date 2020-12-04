/* ******************************************************************
 * libzstd-seek
 * Copyright (c) 2020, Martinelli Marco
 *
 * You can contact the author at :
 * - Source repository : https://github.com/martinellimarco/libzstd-seek
 *
 * This source code is licensed under both the MIT license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv3 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
****************************************************************** */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include "../zstd-seek.h"

void printJumpTable(ZSTDSeek_JumpTable *jt){
    printf("*** JUMP TABLE ***\n");
    printf("Frame\tCompressed\tUncompressed\n");
    for(uint32_t i = 0; i < jt->length; i++){
        ZSTDSeek_JumpTableRecord r = jt->records[i];
        printf("%5d\t%10lu\t%12lu\t\n", i, r.compressedPos, r.uncompressedPos);
    }
    printf("******************\n");
}

//a minimal tar header, just for example
typedef struct {
    char name[100];
    char _fill[24];
    char size[12];
} tar_header;

void listFileInTar(ZSTDSeek_Context *sctx){
    printf("*** List of the files in the .tar.zst archive ***\n");

    tar_header header;
    size_t outBuffSize = sizeof(tar_header);

    size_t ret;
    size_t offset = 0;
    do{
        if(ZSTDSeek_seek(sctx, offset, SEEK_SET)!=0){
            fprintf(stderr, "Error while seeking\n");
            exit(-1);
        }
        ret = ZSTDSeek_read((uint8_t*)&header, outBuffSize, sctx);
        if(ret<0){
            fprintf(stderr, "Error while reading\n");
            exit(-1);
        }

        if(header.name[0]==0){ // tar ends with a null block
            break;
        }
        header.name[99]=0;//just in case readFromPos returned garbage
        printf("%s - ftell: %ld\n", header.name, ZSTDSeek_tell(sctx));

        size_t blockSize = strtol(header.size, NULL, 8);
        offset += (size_t)((ceil((float)blockSize/512.0)+1)*512);//align to 512 and add the header length
    }while(ret==outBuffSize);
}

int main(int argc, const char** argv) {
    if (argc!=2) {
        fprintf(stderr, "An example program that list the files in a .tar.zst\n");
        fprintf(stderr, "Usage: %s <FILE>.tar.zst\n", argv[0]);
        return 1;
    }

    ZSTDSeek_Context* sctx = ZSTDSeek_createFromFile(argv[1]);
    if(!sctx){
        fprintf(stderr, "Can't create the context\n");
        return -1;
    }

    ZSTDSeek_JumpTable *jt = ZSTDSeek_getJumpTableOfContext(sctx);
    if(!jt){
        fprintf(stderr, "Can't get the jump table from the context\n");
        return -1;
    }
    printJumpTable(jt);

    listFileInTar(sctx);

    ZSTDSeek_free(sctx);

    return 0;
}
