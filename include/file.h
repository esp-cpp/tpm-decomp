#ifndef FILE_H
#define FILE_H

#include "common.h"
#include "psxsdk/libcd.h"

/* The game's file system. Everything lives in four archive files whose disc
 * sectors are compiled in (g_DiscFileSectors); the tables below locate the
 * pieces inside them. They are stored relative to their archive, with the
 * 2-second lead-in (00:02:00 = first sector), and File_Init rebases them to
 * absolute disc positions at boot. */

/* FILE.000: named files (levels, VRAM images, sound banks, load screens) */
typedef struct {
    /* 0x00 */ CdlLOC pos;
    /* 0x04 */ u32 size;
    /* 0x08 */ char name[16];
} FileEntry; /* size = 0x18 */

/* FILE.001 .. FILE.003: streams (audio, movies) by index */
typedef struct {
    /* 0x00 */ CdlLOC pos;
    /* 0x04 */ u32 size;
} StreamEntry; /* size = 0x8 */

#define NUM_DISC_FILES 4
#define NUM_FILES 107
#define NUM_STREAMS_001 2624
#define NUM_STREAMS_002 168
#define NUM_STREAMS_003 12

extern u32 g_DiscFileSectors[NUM_DISC_FILES];
extern FileEntry g_FileTable[NUM_FILES];
extern StreamEntry g_StreamTable[NUM_STREAMS_001];
extern StreamEntry g_StreamTable002[NUM_STREAMS_002];
extern StreamEntry g_MovieTable[NUM_STREAMS_003];

void File_RebaseFiles(u32 count, CdlLOC* base, FileEntry* table);
void File_RebaseStreams(u32 count, CdlLOC* base, StreamEntry* table);
void File_Init(void);

#endif
