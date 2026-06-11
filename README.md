# FMFM

A Color Computer 3 diagnostic tool for writing and verifying floppy disk tracks
containing both FM and MFM encoded sectors on the same physical track.

## Purpose

FMFM is a calibration tool, not a data storage utility. Its goal is to determine
the optimal GIME timer delay value for reliably writing a mixed-density track on
real CoCo 3 hardware. A successful run identifies the timer value at which both
the MFM and FM halves of the track write and read back cleanly, with no errors
and no missing sectors.

## Background

Standard CoCo floppy tracks use a single encoding, MFM (double density), for the
entire track. FMFM writes a whole track with 18 MFM
sectors and 17 FM sectors in the other, stopping the FM write halfway
through. This is believed to be the first use of a stock WD179x floppy disk
controller to produce a mixed-density track in this manner.

## Track Layout

| Region | Encoding | Sectors | Bytes/sector |
|--------|----------|---------|--------------|
| First half  | MFM | 18 | 256 |
| Second half | FM  | 17 | 128 (write aborted at sector 9) |

The FM write is intentionally aborted mid-track using the WD179x Force Interrupt
command. Only the first half of the FM sectors are written; the remainder of the
track is left as-is from the MFM write.

## How It Works

### Write phase

Two consecutive Write Track commands are issued:

1. **MFM write** — Full track, hardware-driven. The WD179x halts the CPU via DRQ
   between each byte, so no interrupt interaction is possible. All 18 MFM sectors
   are written.

2. **FM write** — Polled. The CPU actively polls the WD179x for DRQ between bytes.
   A GIME timer interrupt fires mid-track and issues a Force Interrupt command to
   the WD179x, aborting the write.

The timer delay value controls where in the FM region the write is aborted.

### Read phase

After writing, the program performs read passes over the track, scanning for
address marks in both MFM and FM density. For each sector found, it attempts to
read the data field and checks the CRC.

### Display

Results are shown on a 32×16 MC6847 text screen in real time:

```
TRACK 0 SEEN SECTOR STATISTICS
TIMER DELAY: 3140

MFM      123456789111111111
SEC #             012345678
         ------------------
COUNT    ----------77666666
DATA     ----------........

FM       12345678911111111
SEC #             01234567
         -----------------
COUNT    66665555---------
DATA     .......E---------

ENTER NEW DELAY?
```

Each sector position shows one of three states:

| Character | Meaning |
|-----------|---------|
| `digit`   | Address found n times |
| `+`       | Address found more than 10 times |
| `-`       | Address not found |
| `.`       | Data read, CRC clean |
| `E`       | Address / data found, CRC error |

A digit in the COUNT row shows how many times that sector's address mark was seen
on this pass (capped at 9, shown as `+` for 10 or more).

### Tuning loop

After each run the program prompts for a new timer delay value. The operator
adjusts the delay and re-runs until a value is found where:

- All 18 MFM sectors show clean counts (no `-`, no `E`)
- The expected FM sectors (roughly 1–9) show `.`
- The unwritten FM sectors show `-`
- No `E` appears anywhere

## Implementation

FMFM is written in C (compiled with CMOC) and 6809 assembly. The low-level
WD179x write kernel and index pulse synchronization routines are in assembly.
The display, timing logic, and read verification loop are in C.

### Key implementation notes

- The MFM write kernel uses hardware DRQ halting; interrupts need not be masked
  during the MFM phase.
- The FM write kernel is software-polled, making it sensitive to interrupt
  latency. The GIME timer is the only interrupt source active during the FM
  write phase.
- Force Interrupt (WD179x command `0xD8`) is used to cleanly terminate the FM
  write. The chip returns to idle and the read phase begins immediately.

## Requirements

- Color Computer 3
- WD179x-based floppy controller (COCO FDC or compatible)
- Single- and double-density capable floppy drive
- Blank or expendable floppy disk (track 0 will be overwritten)

## Status

Currently tested under MAME 0.288 emulation. Real hardware testing pending.
Results may differ on real hardware, particularly at the FM/MFM transition
boundary, where weak bit effects from the aborted write may cause the boundary
sector to resolve differently on each read pass.