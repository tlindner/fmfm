/* CoCo sector fill/readback utility.  No formatting commands are issued. */
#ifdef __CLANGD__
#define interrupt
#define asm(...)
#endif

#include "x-dskcon-standalone.h"
#include "fm-dskcon-standalone.h"
#include <coco.h>

/* Required by the shared timed-format kernels, unused by this program. */
byte fm_timer_hi;
byte fm_timer_lo;

static byte buffer[1024];
static byte drive;
static byte side;
static byte track;
static byte sector = 1;
static byte mfm = 1;
static byte fill = 0x55;

static void stopMotor(void)
{
    /* Also update BASIC's cached register so its timeout sees the new state. */
    byte control = *(byte *)0x0986 & 0xb0;
    *(byte *)0x0986 = control;
    *(byte *)0xff40 = control;
}

static char *input(void)
{
    char *text = readline();
    if (!text)
    {
        stopMotor();
        exit(1);
    }
    return text;
}

/* Accept decimal, $hex or 0xhex without overflowing a 16-bit CMOC int. */
static unsigned number(const char *prompt, unsigned current, unsigned maximum)
{
    for (;;)
    {
        char *text;
        unsigned value = 0;
        unsigned base = 10;
        byte valid = 1;
        printf("%s (%u)? ", prompt, current);
        text = input();
        if (!*text)
            return current;
        if (*text == '$')
        {
            base = 16;
            ++text;
        }
        else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            base = 16;
            text += 2;
        }
        if (!*text)
            valid = 0;
        while (*text)
        {
            unsigned digit;
            char c = *text++;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            else
            {
                valid = 0;
                break;
            }
            if (digit >= base || value > maximum / base ||
                (value == maximum / base && digit > maximum % base))
            {
                valid = 0;
                break;
            }
            value = value * base + digit;
        }
        if (valid)
            return value;
        printf("ENTER 0-%u, DECIMAL OR $HEX\n", maximum);
    }
}

/* Keep a single session open while scanning IDs.  FM init switches the
 * motor off, so reopening it for every ID can repeatedly sample one sector. */
static unsigned long sessionCookie;
static byte sessionDensity;
static byte savedDriveMask;

static void beginSession(byte density)
{
    disableInterrupts();
    sessionDensity = density;
    /* CoCo uses DS3 as side select when DS0, DS1 or DS2 is asserted.
     * Put it in the mask consumed by every operation, including retries;
     * changing DRGRAM alone would lose it to the kernels' ANDA #$A8. */
    if (density)
    {
        x_partialTrack = 0;
        savedDriveMask = x_dskcon_driveEnableMasks[drive];
        x_dskcon_driveEnableMasks[drive] = savedDriveMask | (side ? 0x40 : 0);
        sessionCookie = x_dskcon_init(x_dskcon_nmiService);
    }
    else
    {
        fm_partialTrack = 0;
        savedDriveMask = fm_dskcon_driveEnableMasks[drive];
        fm_dskcon_driveEnableMasks[drive] = savedDriveMask | (side ? 0x40 : 0);
        fm_dskcon_init(fm_dskcon_nmiService, fm_dskcon_firqService);
    }
    enableInterrupts();
}

static void endSession(void)
{
    disableInterrupts();
    if (sessionDensity)
    {
        x_dskcon_shutdown(sessionCookie);
        x_dskcon_driveEnableMasks[drive] = savedDriveMask;
    }
    else
    {
        fm_dskcon_shutdown();
        fm_dskcon_driveEnableMasks[drive] = savedDriveMask;
    }
    enableInterrupts();
}

static byte perform(byte operation)
{
    if (sessionDensity)
    {
        x_DCOPC = operation;
        x_DCDRV = drive;
        x_DCTRK = track;
        x_DCSEC = sector;
        x_DCBPT = buffer;
        x_dskcon_processSector();
        return x_DCSTA;
    }
    fm_DCOPC = operation;
    fm_DCDRV = drive;
    fm_DCTRK = track;
    fm_DCSEC = sector;
    fm_DCBPT = buffer;
    fm_dskcon_processSector();
    return fm_DCSTA;
}

/* Each session updates BASIC's drive-position table on shutdown. */
static byte transfer(byte operation, byte density)
{
    byte status;
    beginSession(density);
    status = perform(operation);
    endSession();
    return status;
}

/* Restore/step without ID verification, so seeking does not depend on the
 * encoding or contents of tracks crossed on the way to the target. */
static byte positionHead(void)
{
    byte target = track;
    byte status;
    track = 0;
    status = transfer(0, 1);
    while (!status && track < target)
    {
        status = transfer(1, 1);
        if (!status)
            ++track;
    }
    track = target;
    return status;
}

/* Discover the actual size instead of assuming FM=128 or MFM=256 bytes. */
static unsigned sectorSize(void)
{
    unsigned attempt;
    byte lastStatus = 0;
    beginSession(mfm);
    for (attempt = 0; attempt < 100; ++attempt)
    {
        memset(buffer, 0, 6);
        lastStatus = perform(5);
        if (!lastStatus && buffer[0] == track && buffer[1] == side && buffer[2] == sector)
        {
            endSession();
            if (buffer[3] > 3)
            {
                printf("UNSUPPORTED SIZE CODE %u\n", buffer[3]);
                return 0;
            }
            return 128u << buffer[3];
        }
    }
    endSession();
    printf("NO MATCHING VALID ID, STATUS %02X\n", lastStatus);
    printf("LAST ID %02X/%02X/%02X/%02X\n", buffer[0], buffer[1], buffer[2], buffer[3]);
    return 0;
}

int main(void)
{
    printf("SECTOR WRITE/VERIFY\n");
    printf("VALUE: DECIMAL, $HEX OR 0XHEX\n");
    for (;;)
    {
        char *text;
        byte writeSector;
        byte status;
        unsigned size;
        unsigned i;
        printf("W=WRITE R=READ Q=QUIT (W)? ");
        text = input();
        if (!stricmp(text, "Q"))
            break;
        if (!*text || !stricmp(text, "W"))
            writeSector = 1;
        else if (!stricmp(text, "R"))
            writeSector = 0;
        else
            continue;
        drive = (byte)number("DRIVE 0-3", drive, 3);
        for (;;)
        {
            side = (byte)number("SIDE 0-1", side, 1);
            if (drive != 3 || !side)
                break;
            printf("DRIVE 3 SUPPORTS SIDE 0 ONLY\n");
        }
        track = (byte)number("TRACK 0-79", track, 79);
        do
        {
            sector = (byte)number("SECTOR 1-255", sector, 255);
        } while (!sector);
        for (;;)
        {
            printf("MODE FM/MFM (%s)? ", mfm ? "MFM" : "FM");
            text = input();
            if (!*text)
                break;
            if (!stricmp(text, "FM"))
            {
                mfm = 0;
                break;
            }
            if (!stricmp(text, "MFM"))
            {
                mfm = 1;
                break;
            }
        }
        fill = (byte)number(writeSector ? "VALUE 0-255" : "EXPECTED VALUE", fill, 255);
        status = positionHead();
        if (status)
        {
            printf("POSITION FAILED, STATUS %02X\n", status);
            stopMotor();
            continue;
        }
        size = sectorSize();
        if (!size)
        {
            stopMotor();
            continue;
        }
        printf("DRIVE %u SIDE %u TRACK %u\n", drive, side, track);
        printf("SECTOR %u\n", sector);
        printf("%s %u BYTES, FILL $%02X\n", mfm ? "MFM" : "FM", size, fill);
        if (writeSector)
        {
            memset(buffer, fill, size);
            status = transfer(3, mfm);
            printf("WRITE STATUS $%02X\n", status);
            if (status)
            {
                stopMotor();
                continue;
            }
        }
        memset(buffer, (byte)~fill, size);
        status = transfer(2, mfm);
        printf("READ STATUS $%02X\n", status);
        if (!status)
        {
            for (i = 0; i < size && buffer[i] == fill; ++i)
                ;
            if (i == size)
                printf("VERIFIED %u BYTES OF $%02X\n", size, fill);
            else
                printf("MISMATCH AT %u: $%02X\n", i, buffer[i]);
        }
        stopMotor();
    }
    stopMotor();
    return 0;
}
