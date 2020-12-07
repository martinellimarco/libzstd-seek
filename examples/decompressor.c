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
#include <stdlib.h>
#include <string.h>
#include "../zstd-seek.h"

#define BUFFSIZE (128*1024)

static char* createOutFilename(const char* filename){
    size_t const inL = strlen(filename);
    size_t const outL = inL - 4;
    char* const outSpace = (char*)malloc(outL+1);
    memset(outSpace, 0, outL);
    strncat(outSpace, filename, outL);
    outSpace[outL] = 0;
    return (char*)outSpace;
}

int main(int argc, const char** argv) {
    if (argc!=2) {
        fprintf(stderr, "An simple zstd decompressor.\n");
        fprintf(stderr, "Usage: %s <FILE>.zst\n", argv[0]);
        return 1;
    }

    ZSTDSeek_Context* sctx = ZSTDSeek_createFromFileWithoutJumpTable(argv[1]);
    if(!sctx){
        fprintf(stderr, "Can't create the context\n");
        return -1;
    }

    const char* outFileName = createOutFilename(argv[1]);
    FILE* outF = fopen(outFileName, "wb");
    if(!outF){
        fprintf(stderr, "Can't open out file %s", outFileName);
        return -1;
    }
    printf("Decompressing to %s\n", outFileName);
    free((void*)outFileName);

    uint8_t *buff[BUFFSIZE];
    size_t len;
    size_t total=0;
    int i=0;
    while((len = ZSTDSeek_read(buff, BUFFSIZE, sctx))>0){
        total += len;
        if((i++)%10==0){
            printf("\rWrote %.2f MiB", total / 1024.f / 1024.f);
        }
        fwrite(buff, len, 1, outF);
    }
    printf("\rWrote %.2f MiB\n", total / 1024.f / 1024.f);

    fclose(outF);
    ZSTDSeek_free(sctx);

    return 0;
}
