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
		exit(0);
	}
	
    /* Gap IV: fill to end of track */
    while (pos < size)
        buf[pos++] = gap_byte;

    return pos;
}

void processError()
{
	printf("ERROR: $%02x\n", fm_DCSTA);
	exit(0);
}

void formatFM()
{
    const unsigned long cookie = fm_dskcon_init(fm_dskcon_nmiService);

	fm_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	fm_DCDRV = driveNum;      // 0..3
	fm_DCTRK = 0;     // >= 0
	fm_DCSEC = 0;      // >= 1
	
	printf("SEEKING TRACK 0... ");
	fm_dskcon_processSector(); /* seek to track 0 */
	if (fm_DCSTA != 0)
		processError();

	printf("SUCCESS\n");

	fm_DCOPC = 4;      
	fm_DCBPT = track_buf;  // address of sector buffer

	unsigned result;
	result = build_track(track_buf, 6400/2, fm_track_template, /*track*/0, /*side*/0,
		/*fill*/0x55, /*sectors_per_track*/17, /*sector_size_code*/0, 0xff);

	fm_DCTRK = 0;     // >= 0
	printf("FORMATTING FM TRACK... ");
	fm_dskcon_processSector();
	
	if (fm_DCSTA != 0)
		processError();

	printf("SUCCESS\n");
	
	fm_dskcon_shutdown(cookie);
}

void fm_readSectorAddress(byte fm_buf[])
{
    const unsigned long cookie = fm_dskcon_init(fm_dskcon_nmiService);

	fm_DCOPC = 5;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	fm_DCDRV = driveNum;      // 0..3
	fm_DCTRK = 0;     // >= 0
	fm_DCSEC = 0;      // >= 1
	fm_DCBPT = track_buf;  // address of sector buffer

	int i;
	
	for( i=0; i<50; i++)
	{	
		fm_dskcon_processSector(); /* seek to track 0 */
		fm_buf[track_buf[2]]++;
		if(fm_DCSTA != 0 )
		{
			fm_buf[track_buf[2]] = 0xff;
		}

		locate(0,11);
		printf("%c",progress[prog++]);
		if(prog>6) prog = 0;
	}

	fm_dskcon_shutdown(cookie);

	locate(0,11);
	printf(" ");
	
	return;
}

void mfm_readSectorAddress(byte mfm_buf[])
{
    const unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);

	x_DCOPC = 5;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum;      // 0..3
	x_DCTRK = 0;     // >= 0
	x_DCSEC = 0;      // >= 1
	x_DCBPT = track_buf;  // address of sector buffer
	
	int i;
	prog = 0;
	for( i=0; i<50; i++)
	{	
		x_dskcon_processSector(); /* seek to track 0 */
		mfm_buf[track_buf[2]]++;
		if(fm_DCSTA != 0 )
		{
			mfm_buf[track_buf[2]] = 0xff;
		}
		
		locate(0,5);
		printf("%c",progress[prog++]);
		if(prog>6) prog = 0;
	}

	x_dskcon_shutdown(cookie);

	locate(0,5);
	printf(" ");
	
	return;
}

void mfm_readSector(byte mfm_buf[])
{
    const unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);

	x_DCOPC = 2;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum;      // 0..3
	x_DCTRK = 0;     // >= 0
	x_DCBPT = track_buf;  // address of sector buffer
	
	byte i;
	for (i=0; i<20; i++)
	{
		if (mfm_buf[i]>0)
		{
			x_DCSEC = i;      // >= 1
			x_dskcon_processSector();
			locate(i+6,7);
			if(fm_DCSTA != 0 )
			{
				printf("E");
			}
			else
			{
				printf(".");
			}
		}
	}

	x_dskcon_shutdown(cookie);

	return;
}

void fm_readSector(byte fm_buf[])
{
    const unsigned long cookie = fm_dskcon_init(fm_dskcon_nmiService);

	fm_DCOPC = 2;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	fm_DCDRV = driveNum;      // 0..3
	fm_DCTRK = 0;     // >= 0
	fm_DCBPT = track_buf;  // address of sector buffer
	
	byte i;
	for (i=0; i<20; i++)
	{
		if (fm_buf[i]>0)
		{
			fm_DCSEC = i;      // >= 1
			fm_dskcon_processSector();
			locate(i+6,13);
			if(fm_DCSTA != 0 )
			{
				printf("E");
			}
			else
			{
				printf(".");
			}
		}
	}

	fm_dskcon_shutdown(cookie);

	return;
}

void formatMFM()
{
    const unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);

	x_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum;      // 0..3
	x_DCTRK = 0;     // >= 0
	x_DCSEC = 0;      // >= 1
	
	printf("SEEKING TRACK 0... ");
	x_dskcon_processSector(); /* seek to track 0 */
	if (x_DCSTA != 0)
		processError();

	printf("SUCCESS\n");

	x_DCOPC = 4;      
	x_DCBPT = track_buf;  // address of sector buffer

	unsigned result;
	result = build_track(track_buf, 6400, mfm_track_template, /*track*/0, /*side*/0,
		/*fill*/0x55, /*sectors_per_track*/18, /*sector_size_code*/1, GAP_BYTE);

	x_DCTRK = 0;     // >= 0
	printf("FORMATTING MFM TRACK... ");
	x_dskcon_processSector();
	
	if (x_DCSTA != 0)
		processError();

	printf("SUCCESS\n");
	
	fm_dskcon_shutdown(cookie);
}

interrupt void FIRQRoutine(void)
{
    asm
    {
    	
        lda     #$d8          // Load force interrupt command
        sta     FDCREG        // store it
        clr		$ff93         // stop timer
    }
}

void setGIMETimer(unsigned value)
{
    asm
    {
        ldd     :value
        stb     $FF95
        sta     $FF94
    }
}

void setupFIRQ(void)
{
	asm
	{
		lda #$20 /* timer FIRQ enable */
		sta $ff93
		lda #$cc
		ora #$10
		sta $ff90
		lda $ff93 /* clear pending */
	}
}

void stopFIRQ(void)
{
	asm
	{
		lda #$00
		sta $ff93
		lda #$cc
		sta $ff90
		lda $ff93 /* clear pending */
	}
}

void stopMotor(void)
{
	asm
	{
		lda     fm_DRGRAM
		anda    #$B0        ; turn off motors and drive selects
		sta     fm_DRGRAM
		lda     x_DRGRAM
		anda    #$B0        ; turn off motors and drive selects
		sta     x_DRGRAM
		sta     DSKREG
	}
}

int
main()
{
	re_ask:
	printf("DRIVE NUMBER FOR TRACK 0\nTESTING? ");
	char *response = readline();
	int n = atoi(response);
	driveNum = (byte)n;
	
	if (driveNum < 0 || driveNum>3) goto re_ask;
	
	while(1)
	{
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
		printf("TRACK 0 SEEN SECTOR STATISTICS\n");
		printf("TIMER DELAY: %d\n", ((unsigned)fm_timer_hi << 8) | fm_timer_lo);
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
		
		byte i;
		for(i=0; i<20; i++)
		{
			mfm_buf[i]=0;
			fm_buf[i]=0;
		}
		
		mfm_readSectorAddress(mfm_buf);
	
		for(i=1; i<20; i++)
		{
			locate(i+6,6);
			if( mfm_buf[i] == 0)
			{
			}
			else if( mfm_buf[i]<10 )
			{
				printf("%d", mfm_buf[i]);
			}
			else if( mfm_buf[i] == 0xff)
			{
				printf("E");
			}
			else
			{
				printf("+");
			}
		}
	
		mfm_readSector(mfm_buf);
		
		fm_readSectorAddress(fm_buf);
	
		for(i=1; i<20; i++)
		{
			locate(i+6,12);
			if( fm_buf[i] == 0)
			{
			}
			else if( fm_buf[i]<10 )
			{
				printf("%d", fm_buf[i]);
			}
			else if( fm_buf[i] == 0xff)
			{
				printf("E");
			}
			else
			{
				printf("+");
			}
		}
	
		fm_readSector(fm_buf);
	
		locate(0,15);
		printf("ENTER NEW DELAY? ");
		response = readline();
		n = atoi(response);
		
		if (n==0) break;
		
		fm_timer_hi = (byte)(n >> 8);
		fm_timer_lo = (byte)n;
	}
	
	stopMotor();
	
	return 0;
}
