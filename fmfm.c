#include "x-dskcon-standalone.h"
#include <coco.h>

byte track_buf[6400];

/* WD179x special write-track bytes */
#define WT_SYNC   0xF5
#define WT_CRC    0xF7
#define WT_IDAM   0xFE
#define WT_DAM    0xFB
#define GAP_BYTE  0x4E

/* Sentinel values in the count field */
#define SPLICE_ID    0xFD   /* splice: emit track, side, sector, size-code */
#define SPLICE_DATA  0xFC   /* splice: emit SECTOR_SIZE bytes of sector data */
#define DATA_END     0xff

typedef struct {
    byte count;
    byte value;
} TrackEntry;

static const TrackEntry mfm_track_template[] = {
    {  8, 0x00 },       /* Sync field */
    {  3, WT_SYNC },    /* Write A1 sync bytes */
    {  1, WT_IDAM },    /* ID Address Mark */
    { SPLICE_ID, 0 },   /* >> track, side, sector, size-code spliced here << */
    {  1, WT_CRC },     /* Write CRC */
    { 22, GAP_BYTE },   /* Gap II (post-ID gap) */
    { 12, 0x00 },       /* Sync field */
    {  3, WT_SYNC },    /* Write A1 sync bytes */
    {  1, WT_DAM },     /* Data Address Mark */
    { SPLICE_DATA, 0 }, /* >> 256 bytes of sector data spliced here << */
    {  1, WT_CRC },     /* Write CRC */
    { 24, GAP_BYTE },   /* Gap III (post-data gap) */
    { DATA_END, 0 }
};

static const TrackEntry fm_track_template[] = {
    {  6, 0x00 },       /* Sync field */
    {  1, WT_IDAM },    /* ID Address Mark (0xFE) */
    { SPLICE_ID, 0 },   /* >> track, side, sector, size-code spliced here << */
    {  1, WT_CRC },     /* Write CRC */
    { 11, 0xFF },       /* Gap II (post-ID gap) */
    {  6, 0x00 },       /* Sync field */
    {  1, WT_DAM },     /* Data Address Mark (0xFB) */
    { SPLICE_DATA, 0 }, /* >> 256 bytes of sector data spliced here << */
    {  1, WT_CRC },     /* Write CRC */
    { 27, 0xFF },       /* Gap III (post-data gap) */
    { DATA_END, 0 }
};

static void emit(byte *buf, unsigned *pos, byte b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        buf[(*pos)++] = b;
}

/*
 * Build a complete WD179x Write Track buffer for one track.
 *
 * buf     : destination buffer (~6400 bytes for 18-sector DD)
 * track   : track number embedded in all ID fields
 * side    : side number embedded in all ID fields
 * fill    : byte fill sectors
 *
 * Returns total bytes written.
 */
unsigned build_track(byte *buf, int size,
                     const TrackEntry track_template[],
                     byte track, byte side, byte fill,
                     byte sectors_per_track, byte sector_size_code)
{
    unsigned pos = 0;
    unsigned s, i, b;
	unsigned sector_size = 128 << sector_size_code;
	
	/* count entries */
	unsigned nentries = 0;
	
	while (track_template[nentries].count != DATA_END)
	{
		nentries++;
	}	

    /* Gap I (index gap) */
    emit(buf, &pos, GAP_BYTE, 32);

    for (s = 0; s < sectors_per_track; s++) {
        for (i = 0; i < nentries; i++) {
            byte val = track_template[i].value;
            unsigned count = track_template[i].count;

            /* Skip Gap III after the last sector */
            if (s == sectors_per_track - 1 &&
                i == nentries - 1)
                continue;

            switch (count) {
                case SPLICE_ID:
                    buf[pos++] = track;
                    buf[pos++] = side;
                    buf[pos++] = (byte)(s + 1);  /* sector number, 1-based */
                    buf[pos++] = sector_size_code;
                    break;

                case SPLICE_DATA:
                    for (b = 0; b < sector_size; b++)
                        buf[pos++] = fill;
                    break;

                default:
                    emit(buf, &pos, val, count ? count : (unsigned int)256);
                    break;
            }
        }
    }

	if (pos>size)
	{
		printf( "BUFFER OVERFLOW BY: %d BYTES\n", pos-size);
		exit(0);
	}
	
    /* Gap IV: fill to end of track */
    while (pos < size)
        buf[pos++] = GAP_BYTE;

    return pos;
}

void processError()
{
	printf("ERROR: %x\n", x_DCSTA);
	exit(0);
}

int
main()
{
    const unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);

	x_DCOPC = 0;      // 0 = seek to track 0, 2 = read, 3 = write, 4 = format
	x_DCDRV = 1;      // 0..3
	x_DCTRK = 0;     // >= 0
	x_DCSEC = 0;      // >= 1
	
	x_dskcon_processSector();

	x_DCOPC = 4;      // 2 = read, 3 = write, 4 = format
	x_DCDRV = 1;      // 0..3
	x_DCSEC = 0;      // >= 1
	x_DCBPT = track_buf;  // address of sector buffer

	byte i;
	for(i=0; i<35; i++)
	{
		printf("TRACK: %d\n", i);
		unsigned result;
		result = build_track(track_buf, 6400, mfm_track_template, /*track*/i, /*side*/0,
			/*fill*/0x55, /*sectors_per_track*/18, /*sector_size_code*/1);
	
		x_DCTRK = i;     // >= 0
		x_dskcon_processSector();
		
		if (x_DCSTA != 0)
			processError();
	}

	x_dskcon_shutdown(cookie);

	return 0;
}
