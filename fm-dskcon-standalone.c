/*  dskcon-standalone.c - CoCo 4-drive floppy disk sector read/write support.
    By Pierre Sarrazin <http://sarrazip.com/>

    This file is in the public domain, except the parts taken from DECB.
*/

#include "fm-dskcon-standalone.h"

#include <coco.h>

byte fm_DCOPC;  /* DSKCON OPERATION CODE 0-3 */
byte fm_DCDRV;  /* DSKCON DRIVE NUMBER 0—3 */
byte fm_DCTRK;  /* DSKCON TRACK NUMBER 0—34 */
byte fm_DCSEC;  /* DSKCON SECTOR NUMBER 1-18 */
byte *fm_DCBPT; /* DSKCON DATA POINTER */
byte fm_DCSTA;  /* DSKCON STATUS BYTE */
;            /* 0x80 Read Sector */
;            /* 0xa0 Write Sector */
;            /* 0xe0 Read Address */
;            /* 0xf0 Write Track */

byte fm_RDYTMR;    /* MOTOR TURN OFF TIMER */
byte fm_DRGRAM;    /* RAM IMAGE OF DSKREG ($FF40) */
byte fm_DR0TRK[4];    /* CURRENT TRACK NUMBER, DRIVES 0,1,2,3 */
byte fm_NMIFLG;    /* NMI FLAG: 0=DON'T VECTOR <>0=YECTOR OUT */
void *fm_DNMIVC;   /* NMI VECTOR: WHERE TO JUMP FOLLOWING AN NMI */

byte fm_dskcon_driveEnableMasks[4] = { 0x01, 0x02, 0x04, 0x40 };


unsigned long fm_dskcon_init(fm_dskcon_NmiServiceFunctionPointer newNMIService)
{
    fm_DCOPC = 0;
    fm_DCDRV = 0;
    fm_DCTRK = 0;
    fm_DCSEC = 0;
    fm_DCBPT = 0;
    fm_DCSTA = 0;

    fm_RDYTMR = 0;
    fm_DRGRAM = 0;
    fm_NMIFLG = 0;
    fm_DNMIVC = 0;
    * (word *) fm_DR0TRK = 0;
    * (word *) (fm_DR0TRK + 2) = 0;

    // Redirect NMI to jump at newNMIService.
    //
    byte *isr = * (byte **) 0xFFFC;
    unsigned long retVal = ((unsigned long) isr[0] << 16) | * (word *) (isr + 1);  // save orig 3 bytes
    *isr = 0x7E;  // JMP
    * (fm_dskcon_NmiServiceFunctionPointer *) (isr + 1) = newNMIService;

    return retVal;
}


void fm_dskcon_shutdown(unsigned long initReturnValue)
{
    byte *isr = * (byte **) 0xFFFC;
    isr[0] = (byte) (initReturnValue >> 16);
    * (word *) (isr + 1) = (word) initReturnValue;
}


interrupt asm void fm_dskcon_nmiService()
{
    asm
    {
        LDA     fm_NMIFLG      // GET NMI FLAG
        BEQ     @nmiService_end  // RETURN IF NOT ACTIVE
        LDX     fm_DNMIVC      // GET NEW RETURN VECTOR
        STX     10,S        // STORE AT STACKED PC SLOT ON STACK
        CLR     fm_NMIFLG      // RESET NMI FLAG
@nmiService_end:
    }
}


asm __norts__ void fm_dskcon_processSector()
{
    /* CAUTION:
     * This code uses the notation n+FDCREG because FDCREG+n gets expanded
     * to "FDCREG +n" by cpp, so the +n gets ignored as a comment.
     */
    asm
    {
        PSHS    Y,U     /* Preserve registers used by CMOC calling convention */
        LDA     #$05    /* GET RETRY COUNT AND */
        PSHS    A       /* SAVE IT ON THE STACK */
LD765   CLR     fm_RDYTMR  /* RESET DRIVE NOT READY TIMER */
        LDB     fm_DCDRV   /* GET DRIVE NUMBER */
        LEAX    fm_dskcon_driveEnableMasks
        LDA     fm_DRGRAM  /* GET DSKREG IMAGE */
        ANDA    #$A8    /* KEEP MOTOR STATUS, DOUBLE DENSITY. HALT ENABLE */
        ORA     B,X     /* 'OR' IN DRIVE SELECT DATA */
        ORA     #$20    /* 'OR' IN DOUBLE DENSITY */
        LDB     fm_DCTRK   /* GET TRACK NUMBER */
        CMPB    #22     /* PRECOMPENSATION STARTS AT TRACK 22 */
        BLO     LD77E   /* BRANCH IF LESS THAN 22 */
        ORA     #$10    /* TURN ON WRITE PRECOMPENSATION IF >= 22 */
LD77E   TFR     A,B     /* SAVE PARTIAL IMAGE IN ACCB */
        ORA     #$08    /* 'OR' IN MOTOR ON CONTROL BIT */
        STA     fm_DRGRAM  /* SAVE IMAGE IN RAM */
        STA     DSKREG  /* PROGRAM THE 1793 CONTROL REGISTER */
        BITB    #$08    /* WERE MOTORS ALREADY ON? */
        BNE     LD792   /* DON'T WAIT FOR IT TO COME UP TO SPEED IF ALREADY ON */
        LBSR    LA7D1   /* WAIT A WHILE */
        LBSR    LA7D1   /* WAIT SOME MORE FOR MOTOR TO COME UP TO SPEED */
LD792   BSR     LD7D1   /* WAIT UNTIL NOT BUSY OR TIME OUT */
        BNE     LD7A0   /* BRANCH IF TIMED OUT (DOOR OPEN. NO DISK, NO POWER. ETC.) */
        CLR     fm_DCSTA   /* CLEAR STATUS REGISTER */
;
        LDB     fm_DCOPC   /* GET COMMAND */
;
        /* JUMP TO COMMAND WITHOUT JUMP TABLE AS IN DISK BASIC (FOR RELOCATABILITY). */
        LBEQ    fm_dskcon_cmd0
        CMPB    #2
        LBEQ    fm_dskcon_cmd2
        CMPB    #3
        LBEQ    fm_dskcon_cmd3
        CMPB    #4
        LBNE    LD7A0
        LBSR    fm_dskcon_cmd4
        BRA     LD7A0
fm_dskcon_cmd3
        LBSR    LD7FB   /* COMMAND 3: WRITE SECTOR */
        BRA     LD7A0
fm_dskcon_cmd0
        LBSR    LD7B8   /* COMMAND 0: RESTORE HEAD TO TRACK ZERO */
        BRA     LD7A0
fm_dskcon_cmd2
        LBSR    LD7F8   /* COMMAND 2: READ SECTOR */
;
LD7A0   PULS    A       /* GET RETRY COUNT */        LDB     fm_DCSTA   /* GET STATUS */
        BEQ     LD7B1   /* BRANCH IF NO ERRORS */
        DECA            /* DECREMENT RETRIES COUNTER */
        BEQ     LD7B1   /* BRANCH IF NO RETRIES LEFT */
        PSHS    A       /* SAVE RETRY COUNT ON STACK */
        BSR     LD7B8   /* RESTORE HEAD TO TRACK 0 */
        BNE     LD7A0   /* BRANCH IF SEEK ERROR */
        BRA     LD765   /* GO TRY COMMAND AGAIN IF NO ERROR */
LD7B1   LDA     #120    /* 120*1/60 = 2 SECONDS (1/60 SECOND FOR EACH IRQ INTERRUPT) */
        STA     fm_RDYTMR  /* WAIT 2 SECONDS BEFORE TURNING OFF MOTOR */
        LBRA    @fm_dskcon_end  /* EXIT DSKCON */
;
;
; RESTORE HEAD TO TRACK 0
LD7B8   LEAX    fm_DR0TRK  /* POINT TO TRACK TABLE */
        LDB     fm_DCDRV   /* GET DRIVE NUMBER */
        CLR     B,X     /* ZERO TRACK NUMBER */
        LDA     #$03    /* RESTORE HEAD TO TRACK 0, UNLOAD THE HEAD */
        STA     FDCREG  /* AT START, 30 MS STEPPING RATE */
        EXG     A,A
        EXG     A,A     /* WAIT FOR 1793 TO RESPOND TO COMMAND */
        BSR     LD7D1   /* WAIT TILL DRIVE NOT BUSY */
        BSR     LD7F0   /* WAIT SOME MORE */
        ANDA    #$10    /* 1793 STATUS : KEEP ONLY SEEK ERROR */
        STA     fm_DCSTA   /* SAVE IN DSKCON STATUS */
LD7D0   RTS
;
;
; WAIT FOR THE 1793 TO BECOME UNBUSY. IF IT DOES NOT BECOME UNBUSY,
; FORCE AN INTERRUPT AND ISSUE A 'DRIVE NOT READY' 1793 ERROR.
LD7D1   LDX     #0      /* GET ZERO TO X REGISTER - LONG WAIT */
LD7D3   LEAX    -1,X    /* DECREMENT LONG WAIT COUNTER */
        BEQ     LD7DF   /* IF NOT READY BY NOW, FORCE INTERRUPT */
        LDA     FDCREG  /* GET 1793 STATUS AND TEST */
        BITA    #$01    /* BUSY STATUS BIT */
        BNE     LD7D3   /* BRANCH IF BUSY */
        RTS
LD7DF   LDA     #$D0    /* FORCE INTERRUPT COMMAND - TERMINATE ANY COMMAND */
        STA     FDCREG  /* IN PROCESS. DO NOT GENERATE A 1793 INTERRUPT REQUEST */
        EXG     A,A     /* WAIT BEFORE READING 1793 */
        EXG     A,A
        LDA     FDCREG  /* RESET INTRQ (FDC INTERRUPT REQUEST) */
        LDA     #$80    /* RETURN DRIVE NOT READY STATUS IF THE DRIVE DID NOT BECOME UNBUSY */
        STA     fm_DCSTA   /* SAVE DSKCON STATUS BYTE */
        RTS
;
;
; MEDIUM DELAY
LD7F0   LDX     #8750   /* DELAY FOR A WHILE */
LD7F3   LEAX    -1,X    /* DECREMENT DELAY COUNTER AND */
        BNE     LD7F3   /* BRANCH IF NOT DONE */
        RTS
;
fm_dskcon_cmd4
		lda    #$f0     /* write track */
		jmp    LD800
;
; READ ONE SECTOR
LD7F8   LDA     #$80    /* $80 IS READ FLAG (1793 READ SECTOR) */
LD7FA   FCB     $8C     /* SKIP TWO BYTES (THROWN AWAY CMPX INSTRUCTION) */
; WRITE ONE SECTOR
LD7FB   LDA     #$A0    /* $A0 IS WRITE FLAG (1793 WRITE SECTOR) */
LD800   PSHS    A       /* SAVE READ/WRITE FLAG ON STACK */
        LEAX    fm_DR0TRK  /* POINT X TO TRACK NUMBER TABLE IN RAM */
        LDB     fm_DCDRV   /* GET DRIVE NUMBER */
        ABX             /* POINT X TO CORRECT DRIVE'S TRACK BYTE */
        LDB     ,X      /* GET TRACK NUMBER OF CURRENT HEAD POSITION */
        STB     1+FDCREG    /* SEND TO 1793 TRACK REGISTER */
        CMPB    fm_DCTRK       /* COMPARE TO DESIRED TRACK */
        BEQ     LD82C       /* BRANCH IF ON CORRECT TRACK */
        LDA     fm_DCTRK       /* GET TRACK DESIRED */
        STA     3+FDCREG    /* SEND TO 1793 DATA REGISTER */
        STA     ,X      /* SAVE IN RAM TRACK IMAGE */
        LDA     #$17    /* SEEK COMMAND FOR 1793: DO NOT LOAD THE */
        STA     FDCREG  /* HEAD AT START, VERIFY DESTINATION TRACK, */
        EXG     A,A     /* 30 MS STEPPING RATE - WAIT FOR */
        EXG     A,A     /* VALID STATUS FROM 1793 */
        BSR     LD7D1   /* WAIT TILL NOT BUSY */
        BNE     LD82A   /* RETURN IF TIMED OUT */
        BSR     LD7F0   /* WAIT SOME MORE */
        ANDA    #$18    /* KEEP ONLY SEEK ERROR OR CRC ERROR IN ID FIELD */
        BEQ     LD82C   /* BRANCH IF NO ERRORS - HEAD ON CORRECT TRACK */
        STA     fm_DCSTA   /* SAVE IN DSKCON STATUS */
LD82A   PULS    A,PC
; HEAD POSITIONED ON CORRECT TRACK
LD82C   LDA     fm_DCSEC       /* GET SECTOR NUMBER DESIRED */
        STA     2+FDCREG    /* SEND TO 1793 SECTOR REGISTER */
        LEAX    LD88B,PCR   /* POINT X TO ROUTINE TO BE VECTORED */
        STX     fm_DNMIVC      /* TO BY NMI UPON COMPLETION OF DISK I/O AND SAVE VECTOR */
        LDX     fm_DCBPT       /* POINT X TO I/O BUFFER */
        LDA     FDCREG      /* RESET INTRQ (FDC INTERRUPT REQUEST) */
        LDA     fm_DRGRAM      /* GET DSKREG IMAGE */
        ORA     #$80    /* SET FLAG TO ENABLE 1793 TO HALT 6809 */
        PULS    B       /* GET READ/WRITE COMMAND FROM STACK */
        LDY     #0      /* ZERO OUT Y - TIMEOUT INITIAL VALUE */
        LDU     #FDCREG /* U POINTS TO 1793 INTERFACE REGISTERS */
        COM     fm_NMIFLG  /* NMI FLAG = $FF: ENABLE NMI VECTOR */
        ORCC    #$50    /* DISABLE FIRQ,IRQ */
        STB     FDCREG  /* SEND READ/WRITE COMMAND TO 1793: SINGLE RECORD, COMPARE */
        EXG     A,A     /* FOR SIDE 0, NO 15 MS DELAY, DISABLE SIDE SELECT */
        EXG     A,A     /* COMPARE, WRITE DATA ADDRESS MARK (FB) - WAIT FOR STATUS */
;        CMPB    #$80    /* WAS THIS A READ? */
		BITB    #$20    /* WAS THIS A READ? */
        BEQ     LD875   /* IF SO, GO LOOK FOR DATA */
; WAIT FOR THE 1793 TO ACKNOWLEDGE READY TO WRITE DATA
        LDB     #$02    /* DRQ MASK BIT */
LD85B   BITB    ,U      /* IS 1793 READY FOR A BYTE? (DRQ SET IN STATUS BYTE) */
        BNE     LD86B   /* BRANCH IF SO */
        LEAY    -1,Y    /* DECREMENT WAIT TIMER */
        BNE     LD85B   /* KEEP WAITING FOR THE 1793 DRQ */
LD863   CLR     fm_NMIFLG  /* RESET NMI FLAG */
        ANDCC   #$AF    /* ENABLE FIRQ,IRQ */
        LBRA    LD7DF   /* FORCE INTERRUPT, SET DRIVE NOT READY ERROR */
; WRITE A SECTOR
LD86B   LDB     ,X+         /* GET A BYTE FROM RAM */
        STB     3+FDCREG    /* SEND IT TO 1793 DATA REGISTER */
        STA     DSKREG  /* REPROGRAM FDC CONTROL REGISTER */
        BRA     LD86B   /* SEND MORE DATA */
; WAIT FOR THE 17933 TO ACKNOWLEDGE READY TO READ DATA
LD875   LDB     #$02    /* DRQ MASK BIT */
LD877   BITB    ,U      /* DOES THE 1793 HAVE A BYTE? (DRQ SET IN STATUS BYTE) */
        BNE     LD881   /* YES, GO READ A SECTOR */
        LEAY    -1,Y    /* DECREMENT WAIT TIMER */
        BNE     LD877   /* KEEP WAITING FOR 1793 DRQ */
        BRA     LD863   /* GENERATE DRIVE NOT READY ERROR */
; READ A SECTOR
LD881   LDB     3+FDCREG    /* GET DATA BYTE FROM 1793 DATA REGISTER */
        STB     ,X+         /* PUT IT IN RAM */
        STA     DSKREG  /* REPROGRAM FDC CONTROL REGISTER */
        BRA     LD881   /* KEEP GETTING DATA */
; BRANCH HERE ON COMPLETION OF SECTOR READ/WRITE
LD88B   ANDCC   #$AF    /* ENABLE IRQ, FIRO */
        LDA     FDCREG  /* GET STATUS & KEEP WRITE PROTECT, RECORD TYPE/WRITE */
        ANDA    #$7C    /* FAULT, RECORD NOT FOUND, CRC ERROR OR LOST DATA */
        STA     fm_DCSTA   /* SAVE IN DSKCON STATUS */
        RTS
;
;
LA7D1   LDX     #0      /* GET READY TO WAIT A WHILE */
; DELAY WHILE DECREMENTING X TO ZERO
LA7D3   LEAX    -1,X    /* DECREMENT X */
        BNE     LA7D3   /* BRANCH IF NOT ZERO */
        RTS
;
@fm_dskcon_end:
        PULS    Y,U,PC  /* Restore registers used by CMOC calling convention */
    }
}


asm void fm_dskcon_irqService()
{
    asm
    {
        LDA     fm_RDYTMR      // GET TIMER
        BEQ     @end        // BRANCH IF NOT ACTIVE
        DECA                // DECREMENT THE TIMER
        STA     fm_RDYTMR      // SAVE IT
        BNE     @end        // BRANCH IF NOT TIME TO TURN OFF DISK MOTORS
        LDA     fm_DRGRAM      // = GET DSKREG IMAGE
        ANDA    #$B0        // = TURN ALL MOTORS AND DRIVE SELECTS OFF
        STA     fm_DRGRAM      // = PUT IT BACK IN RAM IMAGE
        STA     DSKREG      // SEND TO CONTROL REGISTER (MOTORS OFF)
@end:
    }
}
