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

byte prog;
const char *progress = "|/-\\";
byte driveNum;
byte trackNum;
byte fmFormatStatus;
byte reverseMode;
byte mfmFill = 0x55;
byte partialFormatStatus;
byte partialAborted;
typedef struct {
    byte ioErrors;
    byte badIds;
    byte status;
    byte id[4];
} AddressStats;
AddressStats mfmStats;
AddressStats fmStats;
byte formatStarted;

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
    { SPLICE_DATA, 0 }, /* >> 128 bytes of sector data spliced here << */
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

static void emit(byte *buf, unsigned *pos, unsigned size, byte b, unsigned n)
{
    unsigned i;
    if (*pos > size || n > size - *pos)
    {
        printf("TRACK BUFFER TOO SMALL\n");
        stopMotor();
        exit(1);
    }
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
    unsigned s, i;
	unsigned sector_size = 128 << sector_size_code;

	/* count entries */
	unsigned nentries = 0;

	while (track_template[nentries].count != DATA_END)
	{
		nentries++;
	}

    /* Gap I (index gap) */
    emit(buf, &pos, size, gap_byte, 32);

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
                    emit(buf, &pos, size, track, 1);
                    emit(buf, &pos, size, side, 1);
                    emit(buf, &pos, size, (byte)(s + 1), 1);
                    emit(buf, &pos, size, sector_size_code, 1);
                    break;

                case SPLICE_DATA:
                    emit(buf, &pos, size, fill, sector_size);
                    break;

                default:
                    emit(buf, &pos, size, val, count ? count : (unsigned int)256);
                    break;
            }
        }
    }

    /* Gap IV: fill to end of transfer buffer.  Each F7 command consumes
     * two on-disk byte times, so buffer length is not track length. */
    emit(buf, &pos, size, gap_byte, size - pos);

    return pos;
}

void processError(byte error)
{
	printf("\nERROR: $%02x\n", error);
	stopMotor();
	exit(1);
}

void setGIMETimer(unsigned value);
void stopFIRQ(void);
void setupFIRQ(void);

void formatFM(byte partial)
{
    build_track(track_buf, 6400/2, fm_track_template, trackNum, /*side*/0,
        /*fill*/0x55, /*sectors_per_track*/(partial ? 17 : 16), /*sector_size_code*/0, 0xff);
    fm_partialTrack = partial;
    fm_partialAborted = 0;
    fm_dskcon_init(fm_dskcon_nmiService, fm_dskcon_firqService);

	fm_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	fm_DCDRV = driveNum; // 0..3
	fm_DCTRK = trackNum;     // >= 0
	fm_DCSEC = 0;      // >= 1

	fm_DCOPC = 4;
	fm_DCBPT = track_buf;  // address of sector buffer



	printf("FORMATTING FM TRACK... ");
	fm_dskcon_processSector();

    disableInterrupts();
    setGIMETimer(0);
    stopFIRQ();
    enableInterrupts();
    fmFormatStatus = fm_DCSTA; /* Preserve the complete DSKCON status. */
	
    if (partial)
    {
        partialFormatStatus = fm_DCSTA;
        partialAborted = fm_partialAborted;
    }
	if (fm_DCSTA & (partial ? ~0x04 : 0xff))
	{
		fm_dskcon_shutdown();
		processError(fm_DCSTA);
	}
	
	printf("DONE, STATUS $%02X\n", fmFormatStatus);
	
	fm_dskcon_shutdown();
}

static void recordAddress(byte counts[], byte status, byte sizeCode, AddressStats *stats)
{
    byte sector = track_buf[2];
    stats->status = status;
    memcpy(stats->id, track_buf, 4);
    if (status)
    {
        ++stats->ioErrors;
        return;
    }
    if (track_buf[0] != trackNum || track_buf[1] != 0 ||
        sector < 1 || sector > (sizeCode ? 18 : 17) || track_buf[3] != sizeCode)
    {
        ++stats->badIds;
        return;
    }
    ++counts[sector];
}

static char dataResult(byte status, unsigned size, byte fill)
{
    unsigned i;
    if (status)
        return 'E'; /* Any controller error, not necessarily a CRC error. */
    for (i = 0; i < size; ++i)
        if (track_buf[i] != fill)
            return 'M';
    return '.';
}

void fm_readSectorAddress(byte fm_buf[])
{
    fm_dskcon_init(fm_dskcon_nmiService, fm_dskcon_firqService);
	fm_DCOPC = 5;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	fm_DCDRV = driveNum; // 0..3
	fm_DCTRK = trackNum;     // >= 0
	fm_DCSEC = 0;      // >= 1
	fm_DCBPT = track_buf;  // address of sector buffer

	int i;
	prog = 0;

	for (i=0; i<50; i++)
	{
        memset(track_buf, 0, 6);
        fm_dskcon_processSector();
        recordAddress(fm_buf, fm_DCSTA, 0, &fmStats);

		locate(0,11);
		printf("%c",progress[prog++]);
		if(prog >= 4) prog = 0;

	}

	locate(0,11);
	printf(" ");

	fm_dskcon_shutdown();
}

void mfm_readSectorAddress(byte mfm_buf[])
{
    unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);

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
        memset(track_buf, 0, 6);
        x_dskcon_processSector();
        recordAddress(mfm_buf, x_DCSTA, 1, &mfmStats);

		locate(0,5);
		printf("%c", progress[prog++]);
		if (prog >= 4) prog = 0;

	}

	locate(0,5);
	printf(" ");

	x_dskcon_shutdown(cookie);
}

void mfm_readSector(byte mfm_buf[])
{
    unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);

	x_DCOPC = 2;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum;      // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCBPT = track_buf;  // address of sector buffer

	byte i;
	for (i=1; i<=18; i++)
	{
		if (mfm_buf[i]>0)
		{
            x_DCSEC = i;
            memset(track_buf, 0, 256);
            x_dskcon_processSector();
            locate(i+6,7);
            printf("%c", dataResult(x_DCSTA, 256, mfmFill));
		}
	}

	x_dskcon_shutdown(cookie);
}

void fm_readSector(byte fm_buf[])
{
    fm_dskcon_init(fm_dskcon_nmiService, fm_dskcon_firqService);

	fm_DCOPC = 2;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	fm_DCDRV = driveNum; // 0..3
	fm_DCTRK = trackNum;     // >= 0
	fm_DCBPT = track_buf;  // address of sector buffer

	byte i;
	for (i=1; i<=17; i++)
	{
		if (fm_buf[i]>0)
		{
            fm_DCSEC = i;
            memset(track_buf, 0, 128);
            fm_dskcon_processSector();
            locate(i+6,13);
            printf("%c", dataResult(fm_DCSTA, 128, 0x55));
		}
	}

	fm_dskcon_shutdown();
}

void formatMFM(byte partial)
{
    build_track(track_buf, 6400, mfm_track_template, trackNum, /*side*/0,
        /*fill*/mfmFill, /*sectors_per_track*/18, /*sector_size_code*/1, GAP_BYTE);
    disableInterrupts();
    x_partialTrack = partial;
    x_partialAborted = 0;
    unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);
    if (partial)
    {
        setGIMETimer(0);
        setupFIRQ();
    }
    enableInterrupts();

	x_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
					  // 3 = write, 4 = format, 5 = read address
	x_DCDRV = driveNum; // 0..3
	x_DCTRK = trackNum;     // >= 0
	x_DCSEC = 0;      // >= 1
	x_DCOPC = 4;
	x_DCBPT = track_buf;  // address of sector buffer



	x_DCTRK = trackNum;     // >= 0
	printf("FORMATTING MFM TRACK... ");
	x_dskcon_processSector();

    disableInterrupts();
    if (partial)
    {
        setGIMETimer(0);
        stopFIRQ();
        partialFormatStatus = x_DCSTA;
        partialAborted = x_partialAborted;
    }
    x_dskcon_shutdown(cookie);
    x_partialTrack = 0;
    enableInterrupts();
	if (x_DCSTA & (partial ? ~0x04 : 0xff))
	{
		processError(x_DCSTA);
	}

	printf("DONE, STATUS $%02X\n", x_DCSTA);
}

/* Do not overwrite the baseline until we know the full MFM format worked.
 * This also distinguishes transfer failures from FM splice/timer failures. */
static void verifyMFMFormat(void)
{
    unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);
    byte sector;
    x_DCOPC = 2;
    x_DCDRV = driveNum;
    x_DCTRK = trackNum;
    x_DCBPT = track_buf;
    printf("VERIFYING MFM BEFORE FM...\n");
    for (sector = 1; sector <= 18; ++sector)
    {
        char result;
        x_DCSEC = sector;
        memset(track_buf, 0, 256);
        x_dskcon_processSector();
        result = dataResult(x_DCSTA, 256, mfmFill);
        if (result != '.')
        {
            byte status = x_DCSTA;
            x_dskcon_shutdown(cookie);
            printf("MFM BASELINE FAILED SEC %u\n", sector);
            printf("STATUS %02X RESULT %c\n", status, result);
            printf("FM WRITE WAS NOT STARTED\n");
            stopMotor();
            exit(1);
        }
    }
    x_dskcon_shutdown(cookie);
    printf("MFM BASELINE: 18 SECTORS OK\n");
}

static void verifyFMFormat(void)
{
    byte sector;
    fm_dskcon_init(fm_dskcon_nmiService, fm_dskcon_firqService);
    fm_DCOPC = 2;
    fm_DCDRV = driveNum;
    fm_DCTRK = trackNum;
    fm_DCBPT = track_buf;
    printf("VERIFYING FM BEFORE MFM...\n");
    for (sector = 1; sector <= 16; ++sector)
    {
        char result;
        fm_DCSEC = sector;
        memset(track_buf, 0, 128);
        fm_dskcon_processSector();
        result = dataResult(fm_DCSTA, 128, 0x55);
        if (result != '.')
        {
            byte status = fm_DCSTA;
            fm_dskcon_shutdown();
            printf("FM BASELINE FAILED SEC %u\n", sector);
            printf("STATUS %02X RESULT %c\n", status, result);
            printf("MFM WRITE WAS NOT STARTED\n");
            stopMotor();
            exit(1);
        }
    }
    fm_dskcon_shutdown();
    printf("FM BASELINE: 16 SECTORS OK\n");
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
		lda #$00     /* turn off all FIRQs */
		sta $ff93
		lda #$cc     /* Load standard config */
		sta $ff90
		lda $ff93    /* clear pending */
	}
#endif
}

void armMotorTimeout(byte seconds)
{
    /* Poke Disk BASIC's RDYTMR at $0985.
     * Value  1/60s ticks until motor off. */
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
	else
	{
		printf("+");
	}
}

static unsigned readNumber(const char *prompt, unsigned current, unsigned maximum)
{
    for (;;)
    {
        char *text;
        unsigned value = 0;
        byte valid = 1;
        printf("%s (%u)? ", prompt, current);
        text = readline();
        if (!text)
        {
            stopMotor();
            exit(1);
        }
        if (!*text)
            return current;
        while (*text)
        {
            if (*text < '0' || *text > '9' || value > maximum / 10 ||
                (value == maximum / 10 && (unsigned)(*text - '0') > maximum % 10))
            {
                valid = 0;
                break;
            }
            value = value * 10 + (*text++ - '0');
        }
        if (valid)
            return value;
        printf("ENTER 0-%u\n", maximum);
    }
}

int
main()
{
    driveNum = (byte)readNumber("DRIVE NUMBER", driveNum, 3);

    while (1)
    {
        char *response;
        byte writeTrack;
        unsigned n;
        printf("W=FM OVER MFM B=MFM OVER FM\nR=READ Q=QUIT? ");
        response = readline();
        if (!response)
            break;
        if (*response == 'Q' || *response == 'q')
            break;
        writeTrack = *response == 'W' || *response == 'w' || *response == 'B' || *response == 'b';
        if (writeTrack)
            reverseMode = *response == 'B' || *response == 'b';
        if (!writeTrack && *response && *response != 'R' && *response != 'r')
            continue;
        if (writeTrack)
        {
            n = readNumber("DELAY (0=QUIT)", ((unsigned)fm_timer_hi << 8) | fm_timer_lo, 4095);
            if (!n)
                break;
            fm_timer_hi = (byte)(n >> 8);
            fm_timer_lo = (byte)n;
        }
        if (!writeTrack)
            reverseMode = (byte)readNumber("REVERSE PATTERN (0/1)", reverseMode, 1);
        mfmFill = reverseMode ? 0xaa : 0x55;
        trackNum = (byte)readNumber("TRACK NUMBER", trackNum, 40);

		// go to track zero
		unsigned long cookie = x_dskcon_init(x_dskcon_nmiService);
		x_DCOPC = 0;      // 0 = seek to track 0, 2 = read,
						  // 3 = write, 4 = format, 5 = read address
		x_DCDRV = driveNum; // 0..3
		x_DCTRK = 0;     // >= 0
		x_DCSEC = 0;      // >= 1

		printf("SEEKING TRACK 0... ");
		x_dskcon_processSector(); /* seek to track 0 */
		if (x_DCSTA!=0)
		{
			x_dskcon_shutdown(cookie);
			processError(x_DCSTA);
		}

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
			{
				x_dskcon_shutdown(cookie);
				processError(x_DCSTA);
			}

			printf("SUCCESS: %d\n", x_DCTRK);
		}

		x_dskcon_shutdown(cookie);
		
        formatStarted = 0;
        if (writeTrack)
        {
            formatStarted = 1;
            printf("WRITE SELECTED: DRIVE %u TRACK %u\n", driveNum, trackNum);
            partialAborted = 0;
            if (reverseMode)
            {
                formatFM(0);
                verifyFMFormat();
                formatMFM(1);
            }
            else
            {
                formatMFM(0);
                verifyMFMFormat();
                disableInterrupts();
                setGIMETimer(0);
                setupFIRQ();
                enableInterrupts();
                formatFM(1);
            }
            if (!partialAborted)
            {
                printf("TIMER ABORT DID NOT FIRE\n");
                stopMotor();
                exit(1);
            }
        }

		cls(255);
		printf("DRIVE %d %s\n", driveNum, writeTrack ? "WRITE/VERIFY" : "READ ONLY");
		printf("TRK %u DELAY %u MFM %02X\n", trackNum, ((unsigned)fm_timer_hi << 8) | fm_timer_lo, mfmFill);
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

		memset(&mfmStats, 0, sizeof(mfmStats));
        memset(&fmStats, 0, sizeof(fmStats));
		mfm_readSectorAddress(mfm_buf);

		for (i=1; i<=18; i++)
		{
			displaySectorAddress(i, 6, mfm_buf[i]);
		}

		mfm_readSector(mfm_buf);

		fm_readSectorAddress(fm_buf);

		for (i=1; i<=17; i++)
		{
			displaySectorAddress(i, 12, fm_buf[i]);
		}

		fm_readSector(fm_buf);

        locate(0,2);
        printf("S%02X ID %02X/%02X/%02X/%02X", mfmStats.status,
            mfmStats.id[0], mfmStats.id[1], mfmStats.id[2], mfmStats.id[3]);
        locate(0,5);
        printf("I/O ERR %u BAD ID %u       ", mfmStats.ioErrors, mfmStats.badIds);
        locate(0,8);
        printf("S%02X ID %02X/%02X/%02X/%02X", fmStats.status,
            fmStats.id[0], fmStats.id[1], fmStats.id[2], fmStats.id[3]);
        locate(0,11);
        printf("I/O ERR %u BAD ID %u       ", fmStats.ioErrors, fmStats.badIds);
        locate(0,14);
        printf("MODE %c FORMAT %c %s STAT %02X", writeTrack ? (reverseMode ? 'B' : 'W') : 'R',
            formatStarted ? 'Y' : 'N', reverseMode ? "MFM" : "FM",
            formatStarted ? partialFormatStatus : 0);
        armMotorTimeout(2);

		locate(0,15);
	}

	stopMotor();
	return 0;
}
