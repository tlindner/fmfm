/*  dskcon-standalone.h - CoCo 4-drive floppy disk sector read/write support.
    By Pierre Sarrazin <http://sarrazip.com/>

    This file is in the public domain.
*/

#ifndef _H_x_dskcon
#define _H_x_dskcon

#ifdef __CLANGD__
#define interrupt
#define __norts__
#define asm(...)
#endif

#include <cmoc.h>

extern unsigned char x_DCOPC;  /* DSKCON OPERATION CODE 0-3 */
extern unsigned char x_DCDRV;  /* DSKCON DRIVE NUMBER 0—3 */
extern unsigned char x_DCTRK;  /* DSKCON TRACK NUMBER 0—34 */
extern unsigned char x_DCSEC;  /* DSKCON SECTOR NUMBER 1-18 */
extern unsigned char *x_DCBPT; /* DSKCON DATA POINTER */
extern unsigned char x_DCSTA;  /* DSKCON STATUS BYTE */

extern unsigned char x_RDYTMR;    /* MOTOR TURN OFF TIMER */
extern unsigned char x_DRGRAM;    /* RAM IMAGE OF DSKREG ($FF40) */
extern unsigned char x_DR0TRK[4];    /* CURRENT TRACK NUMBER, DRIVES 0,1,2,3 */
extern unsigned char x_NMIFLG;    /* NMI FLAG: 0=DON'T VECTOR <>0=YECTOR OUT */
extern void *x_DNMIVC;   /* NMI VECTOR: WHERE TO JUMP FOLLOWING AN NMI */

extern unsigned char x_dskcon_driveEnableMasks[4];

asm x_clear_drgram(void);

// Type used by x_dskcon_init().
//
typedef interrupt void (*x_dskcon_NmiServiceFunctionPointer)();


// Function to be called first.
// Initializes the floppy disk sector read/write support.
// Must be called while interrupts are masked.
//
// The IRQ service routine must be coded so that it invokes
// x_dskcon_irqService().  Call x_dskcon_processSector() to read/write a sector.
//
// newNMIService: Address of the nom-maskable interrupt service routine.
//                Typically, this is the address of x_dskcon_nmiService().
//
// Returns a 24-bit value that must be passed to x_dskcon_shutdown()
// to restore the original NMI service routine.
//
unsigned long x_dskcon_init(x_dskcon_NmiServiceFunctionPointer newNMIService);


// initReturnValue: Must be the value obtained from dskcon_init.
// Must be called while interrupts are masked.
//
void x_dskcon_shutdown(unsigned long initReturnValue);


// Function to be used as the NMI service routine.
//
interrupt void x_dskcon_nmiService();


// Equivalent of DSKCON.
// Fill x_DCOPC, etc., then call this.
// Upon exit, x_DCSTA contains these bits:
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
// Call x_dskcon_init() before calling this.
//
// Example:
//     x_DCOPC = 2;      // 2 = read, 3 = write
//     x_DCDRV = 0;      // 0..3
//     x_DCTRK = 17;     // >= 0
//     x_DCSEC = 3;      // >= 1
//     x_DCBPT = 0x500;  // address of 256-byte sector buffer
//     x_dskcon_processSector();
//     if (x_DCSTA != 0)
//         processError();
//
// The name was chosen to avoid clashing with the dskcon() defined
// by CMOC's standard library function dskcon().
//
void x_dskcon_processSector();


// Function to be called by the CoCo's IRQ service routine.
// This is why this function does not itself have the 'interrupt' keyword.
// Example:
// interrupt void irqService()
// {
//     if ((* (char *) 0xFF03) & 0x80)  // do nothing if 63.5 us interrupt
//     {
//         * (char *) 0xFF02;  // 60 Hz interrupt. Reset PIA0, port B interrupt flag.
//         x_dskcon_irqService();
//     }
// }
//
void x_dskcon_irqService();


#endif  /* _H_x_dskcon */
