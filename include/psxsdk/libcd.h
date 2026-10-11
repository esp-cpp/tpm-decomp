#ifndef PSXSDK_LIBCD_H
#define PSXSDK_LIBCD_H

/* The parts of the PlayStation CD library this game uses, declared from the
 * public API (not copied from Sony's headers). */

typedef struct {
    unsigned char minute; /* BCD */
    unsigned char second; /* BCD */
    unsigned char sector; /* BCD */
    unsigned char track;
} CdlLOC;

typedef struct {
    CdlLOC pos;
    unsigned long size;
    char name[16];
} CdlFILE;

CdlLOC* CdIntToPos(int i, CdlLOC* p);
int CdPosToInt(CdlLOC* p);

#endif
