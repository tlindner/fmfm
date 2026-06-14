#ifdef __CLANGD__
#define interrupt
#define asm(...)
#endif

#include "x-dskcon-standalone.h"
#include "fm-dskcon-standalone.h"
#include <coco.h>

byte track_buf[6400];
enum
{
    DSKREG = 0xFF40,   // DISK CONTROL REGISTER
    FDCREG = 0xFF48,   // 1793 CONTROL REGISTER
};

byte *screen_buf = (byte *)0x400;

byte prog;
const char *progress = "\\!/-\\!";
byte driveNum;
byte trackNum;
static unsigned long cookie;

byte fm_timer_hi = 0x0C;
byte fm_timer_lo = 0x44;

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

void stopMotor(void)
{
#ifndef __CLANGD__
	asm
	{
		lda     x_DRGRAM
		anda    #$B0        ; turn off motors and drive selects
		sta     x_DRGRAM
		sta     DSKREG
	}
#endif
}

static void emit(byte *buf, unsigned *pos, byte b, unsigned n)
{
    unsigned i;
    for (i=0; i<n; i++)
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
                     byte sectors_per_track, byte sector_size_code,
                     byte gap_byte)
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
    emit(buf, &pos, gap_byte, 32);

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
		x_dskcon_shutdown(cookie);
		exit(0);
	}
	
    /* Gap IV: fill to end of track */
    while (pos<size)
        buf[pos++] = gap_byte;

    return pos;
}

void processError(byte error)
{
	printf("\nERROR: $%02x\n", error);
	stopMotor();
	x_dskcon_shutdown(cookie);
	exit(0);
}

void formatFM()
{
	x_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum; // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCSEC = 0;      // >= 1
	
// 	printf("SEEKING TRACK 0... ");
// 	fm_dskcon_processSector(); /* seek to track 0 */
// 	if (x_DCSTA!=0)
// 		processError(x_DCSTA);
// 
// 	printf("SUCCESS\n");

	x_DCOPC = 4;      
	x_DCBPT = track_buf;  // address of sector buffer

	unsigned result;
	result = build_track(track_buf, 6400/2, fm_track_template, trackNum, /*side*/0,
		/*fill*/0x55, /*sectors_per_track*/17, /*sector_size_code*/0, 0xff);

// 	x_DCTRK = 0;     // >= 0
	printf("FORMATTING FM TRACK... ");
	fm_dskcon_processSector();
	
	if (x_DCSTA != 0)
		processError(x_DCSTA);

	printf("SUCCESS\n");
}

void fm_readSectorAddress(byte fm_buf[])
{
	x_DCOPC = 5;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum; // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCSEC = 0;      // >= 1
	x_DCBPT = track_buf;  // address of sector buffer

	int i;
	
	for (i=0; i<50; i++)
	{	
		fm_dskcon_processSector(); /* seek to track 0 */
		fm_buf[track_buf[2]]++;
		if (x_DCSTA!=0 )
		{
			fm_buf[track_buf[2]] = 0xff;
		}

		locate(0,11);
		printf("%c",progress[prog++]);
		if(prog>6) prog = 0;
		
		/* bail early if nothing seen after 2 attempts */
		if (i==1)
		{
			byte j, found = 0;
			for (j=1; j<20; j++)
				if (fm_buf[j]) found = 1;
			if (!found) break;
		}
	}

	locate(0,11);
	printf(" ");
	
	return;
}

void mfm_readSectorAddress(byte mfm_buf[])
{
	x_DCOPC = 5;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum; // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCSEC = 0;      // >= 1
	x_DCBPT = track_buf;  // address of sector buffer
	
	int i;
	prog = 0;
	for (i=0; i<50; i++)
	{	
		x_dskcon_processSector(); /* seek to track 0 */
		mfm_buf[track_buf[2]]++;
		if (x_DCSTA != 0)
		{
			mfm_buf[track_buf[2]] = 0xff;
		}
		
		locate(0,5);
		printf("%c", progress[prog++]);
		if (prog>6) prog = 0;

		/* bail early if nothing seen after 2 attempts */
		if (i==1)
		{
			byte j, found = 0;
			for (j=1; j<20; j++)
				if (mfm_buf[j]) found = 1;
			if (!found) break;
		}
	}

	locate(0,5);
	printf(" ");
	
	return;
}

void mfm_readSector(byte mfm_buf[])
{
	x_DCOPC = 2;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum;      // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCBPT = track_buf;  // address of sector buffer
	
	byte i;
	for (i=0; i<20; i++)
	{
		if (mfm_buf[i]>0)
		{
			x_DCSEC = i;      // >= 1
			x_dskcon_processSector();
			locate(i+6,7);
			if (x_DCSTA!=0)
			{
				printf("E");
			}
			else
			{
				printf(".");
			}
		}
	}

	return;
}

void fm_readSector(byte fm_buf[])
{
	x_DCOPC = 2;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum; // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCBPT = track_buf;  // address of sector buffer
	
	byte i;
	for (i=0; i<20; i++)
	{
		if (fm_buf[i]>0)
		{
			x_DCSEC = i;      // >= 1
			fm_dskcon_processSector();
			locate(i+6,13);
			if (x_DCSTA!=0)
			{
				printf("E");
			}
			else
			{
				printf(".");
			}
		}
	}

	return;
}

void formatMFM()
{
	x_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum; // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCSEC = 0;      // >= 1
	
// 	printf("SEEKING TRACK 0... ");
// 	x_dskcon_processSector(); /* seek to track 0 */
// 	if (x_DCSTA != 0)
// 		processError(x_DCSTA);
// 
// 	printf("SUCCESS\n");

	x_DCOPC = 4;      
	x_DCBPT = track_buf;  // address of sector buffer

	unsigned result;
	result = build_track(track_buf, 6400, mfm_track_template, trackNum, /*side*/0,
		/*fill*/0x55, /*sectors_per_track*/18, /*sector_size_code*/1, GAP_BYTE);

	x_DCTRK = trackNum;     // >= 0
	printf("FORMATTING MFM TRACK... ");
	x_dskcon_processSector();
	
	if (x_DCSTA!=0)
		processError(x_DCSTA);

	printf("SUCCESS\n");
}

interrupt void FIRQRoutine(void)
{
#ifndef __CLANGD__
    asm
    {
    	
        lda     #$d8          // Load force interrupt command
        sta     FDCREG        // store it
        clr		$ff93         // stop timer
    }
#endif
}

void setGIMETimer(unsigned value)
{
#ifndef __CLANGD__
    asm
    {
        ldd     :value
        stb     $FF95
        sta     $FF94
    }
#endif
}

void setupFIRQ(void)
{
#ifndef __CLANGD__
	asm
	{
		lda #$20 /* timer FIRQ enable */
		sta $ff93
		lda #$cc
		ora #$10
		sta $ff90
		lda $ff93 /* clear pending */
	}
#endif
}

void stopFIRQ(void)
{
#ifndef __CLANGD__
	asm
	{
		lda #$00
		sta $ff93
		lda #$cc
		sta $ff90
		lda $ff93 /* clear pending */
	}
#endif
}

void armMotorTimeout(byte seconds)
{
    /* Poke Disk BASIC's RDYTMR at $0985.
     * Value × 1/60s ticks until motor off. */
    * (byte *) 0x0985 = (byte)(seconds * 60);
}

void displaySectorAddress(byte offset, byte row, byte value)
{
	locate(offset+6,row);
	if (value==0)
	{
	}
	else if (value<10)
	{
		printf("%d", value);
	}
	else if (value==0xff)
	{
		printf("E");
	}
	else
	{
		printf("+");
	}
}

int
main()
{
    cookie = x_dskcon_init(x_dskcon_nmiService);

	re_ask_drive:
	printf("DRIVE NUMBER (%d)? ", driveNum);
	char *response = readline();
	int n;
	if (response[0] != '\0')
	{
		n = atoi(response);
		driveNum = (byte)n;
		
		if (driveNum<0 || driveNum>3) goto re_ask_drive;
	}

	while (1)
	{
		re_ask_delay:
		printf("ENTER NEW DELAY (%d)? ", ((unsigned)fm_timer_hi << 8) | fm_timer_lo);
		response = readline();
		if (response[0] != '\0')
		{
			n = atoi(response);
			
			if (n<0 || n>4095) goto re_ask_delay;
			
			if (n!=0)
			{
				fm_timer_hi = (byte)(n>>8);
				fm_timer_lo = (byte)n;
			}
			else
			{
				stopMotor();
				break;
			}
		}
	
		re_ask_track:
		printf("TRACK NUMBER (%d)? ", trackNum);
		char *response = readline();
		if (response[0] != '\0')
		{
			n = atoi(response);
			trackNum = (byte)n;
			
			if (trackNum<0 || trackNum>40) goto re_ask_track;
		}

		// go to track zero
		x_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
						  // 3 = write, 4 = format, 5 = read address
		x_DCDRV = driveNum; // 0..3
		x_DCTRK = 0;     // >= 0
		x_DCSEC = 0;      // >= 1
		
		printf("SEEKING TRACK 0... ");
		x_dskcon_processSector(); /* seek to track 0 */
		if (x_DCSTA!=0)
			processError(x_DCSTA);
	
		printf("SUCCESS\n");
		
		// step in trackNum times
		
		x_DCOPC = 1;      // 0 = seek to track 0, 1 = step in, 2 = read,
						  // 3 = write, 4 = format, 5 = read address
		x_DCDRV = driveNum; // 0..3
		x_DCSEC = 0;      // >= 1
		x_DCTRK = 0; // >= 0
		
		byte i;
		for (i=0; i<trackNum; i++)
		{
			printf("STEPPING TO TRACK %d, ", i+1);
			x_dskcon_processSector(); /* step in */
			if (x_DCSTA!=0)
				processError(x_DCSTA);
		
			printf("SUCCESS: %d\n", x_DCTRK);
		}

		formatMFM();
	
		disableInterrupts();
		char *irqVector = * (char **) 0xFFF6;
		*irqVector = 0x7E;  // extended JMP instruction
		* (void **) (irqVector + 1) = (void *) FIRQRoutine;
		setGIMETimer(0);
		setupFIRQ();
		enableInterrupts();
		
		formatFM();
	
		disableInterrupts();
		stopFIRQ();
		enableInterrupts();
		
		cls(255);
		printf("TRACK %d SEEN SECTOR STATISTICS\n", trackNum);
		printf("Drive: %d, TIMER DELAY: %d\n", driveNum, ((unsigned)fm_timer_hi << 8) | fm_timer_lo);
		printf("\n");
		printf("MFM    123456789111111111\n");
		printf("SEC #           012345678\n");
		printf("       ------------------\n");
		printf("COUNT  ------------------\n");
		printf("DATA   ------------------\n");
		printf("\n");
		printf("FM     123456789111111111\n");
		printf("SEC #           012345678\n");
		printf("       ------------------\n");
		printf("COUNT  ------------------\n");
		printf("DATA   ------------------\n");
		
		byte mfm_buf[20];
		byte fm_buf[20];
		
		for (i=0; i<20; i++)
		{
			mfm_buf[i] = 0;
			fm_buf[i] = 0;
		}
		
		mfm_readSectorAddress(mfm_buf);
	
		for (i=1; i<20; i++)
		{
			displaySectorAddress(i, 5, mfm_buf[i]);
		}
	
		mfm_readSector(mfm_buf);
		
		fm_readSectorAddress(fm_buf);
	
		for (i=1; i<20; i++)
		{
			displaySectorAddress(i, 12, fm_buf[i]);
		}
	
		fm_readSector(fm_buf);
	
		armMotorTimeout(2);
		
		locate(0,14);
	}

	x_dskcon_shutdown(cookie);
	
	return 0;
}
