/*  dskcon-standalone.h - CoCo 4-drive floppy disk sector read/write support.
    By Pierre Sarrazin <http://sarrazip.com/>

    This file is in the public domain.
*/

#ifndef _H_fm_dskcon
#define _H_fm_dskcon

#include <cmoc.h>


extern unsigned char fm_DCOPC;  /* DSKCON OPERATION CODE 0-3 */
extern unsigned char fm_DCDRV;  /* DSKCON DRIVE NUMBER 0—3 */
extern unsigned char fm_DCTRK;  /* DSKCON TRACK NUMBER 0—34 */
extern unsigned char fm_DCSEC;  /* DSKCON SECTOR NUMBER 1-18 */
extern unsigned char *fm_DCBPT; /* DSKCON DATA POINTER */
extern unsigned char fm_DCSTA;  /* DSKCON STATUS BYTE */

extern unsigned char fm_RDYTMR;    /* MOTOR TURN OFF TIMER */
extern unsigned char fm_DRGRAM;    /* RAM IMAGE OF DSKREG ($FF40) */
extern unsigned char fm_DR0TRK[4];    /* CURRENT TRACK NUMBER, DRIVES 0,1,2,3 */
extern unsigned char fm_NMIFLG;    /* NMI FLAG: 0=DON'T VECTOR <>0=YECTOR OUT */
extern void *fm_DNMIVC;   /* NMI VECTOR: WHERE TO JUMP FOLLOWING AN NMI */

extern unsigned char fm_dskcon_driveEnableMasks[4];

// Type used by fm_dskcon_init().
//
typedef interrupt void (*fm_dskcon_NmiServiceFunctionPointer)();


// Function to be called first.
// Initializes the floppy disk sector read/write support.
// Must be called while interrupts are masked.
//
// The IRQ service routine must be coded so that it invokes
// fm_dskcon_irqService().  Call fm_dskcon_processSector() to read/write a sector.
//
// newNMIService: Address of the nom-maskable interrupt service routine.
//                Typically, this is the address of fm_dskcon_nmiService().
//
// Returns a 24-bit value that must be passed to fm_dskcon_shutdown()
// to restore the original NMI service routine.
//
unsigned long fm_dskcon_init(fm_dskcon_NmiServiceFunctionPointer newNMIService);


// initReturnValue: Must be the value obtained from dskcon_init.
// Must be called while interrupts are masked.
//
void fm_dskcon_shutdown(unsigned long initReturnValue);


// Function to be used as the NMI service routine.
//
interrupt void fm_dskcon_nmiService();


// Equivalent of DSKCON.
// Fill fm_DCOPC, etc., then call this.
// Upon exit, fm_DCSTA contains these bits:
//
//  Bit  Error Type
//  ===  ==========
//   7   Drive Not Ready
//   6   Write Protect
//   5   Write Fault
//   4   Seek Error or Record Not Found
//   3   CRC Error
//   2   Lost Data
//
// Call fm_dskcon_init() before calling this.
//
// Example:
//     fm_DCOPC = 2;      // 2 = read, 3 = write
//     fm_DCDRV = 0;      // 0..3
//     fm_DCTRK = 17;     // >= 0
//     fm_DCSEC = 3;      // >= 1
//     fm_DCBPT = 0x500;  // address of 256-byte sector buffer
//     fm_dskcon_processSector();
//     if (fm_DCSTA != 0)
//         processError();
//
// The name was chosen to avoid clashing with the dskcon() defined
// by CMOC's standard library function dskcon().
//
void fm_dskcon_processSector();


// Function to be called by the CoCo's IRQ service routine.
// This is why this function does not itself have the 'interrupt' keyword.
// Example:
// interrupt void irqService()
// {
//     if ((* (char *) 0xFF03) & 0x80)  // do nothing if 63.5 us interrupt
//     {
//         * (char *) 0xFF02;  // 60 Hz interrupt. Reset PIA0, port B interrupt flag.
//         fm_dskcon_irqService();
//     }
// }
//
void fm_dskcon_irqService();


#endif  /* _H_fm_dskcon */
