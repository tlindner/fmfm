# FMFM

A Color Computer 3 diagnostic tool for writing and verifying floppy tracks
containing FM and MFM sectors on the same physical track. It uses a WD179x
controller and a GIME timer to tune an interrupted FM format operation.

## Operation

Load the executable with `LOADM "FMFM"` and start it with `EXEC`.
Select a drive (0–3), then choose:

- **W**: format the selected track and verify it. Enter a timer delay (1–4095,
  initially 3140 / `$0C44`) and track number (0–40). Delay 0 exits.
- **B**: reverse write: format 16 FM sectors, verify all 16, then partially
  overwrite with MFM. Uses the same delay and track prompts as W.
- **R** or Enter: read and verify the selected track without formatting it.
  Select reverse pattern **1** for B images, or **0** for W/older images.
  Enter keeps the previous selection in the current program session.
- **Q**: stop the motor and exit.

Write mode overwrites the selected track, side 0. Use an expendable image or
disk, and keep the program disk in another drive when appropriate. Track 40
is intended for drives/media that support it; a standard 40-track disk ends
at track 39. Numeric input is checked before conversion to byte-sized values.

## How the mixed track is produced

1. Write a complete MFM track containing sectors 1–18, each 256 bytes.
2. Read all 18 MFM sectors and verify controller status and `$55` payloads.
   If this fails, print the failing sector/status and stop without starting FM.
3. Prepare an FM template containing sectors 1–17, each 128 bytes.
4. Synchronize using the index indication, wait approximately half a rotation,
   arm the GIME timer, and issue the FM Write Track command. The controller
   starts the actual write at index.
5. The timer FIRQ redirects execution to a routine that issues Force Interrupt
   (`$D8`), stopping the FM write before a complete revolution.

W mode fills both densities with `$55`, preserving compatibility with older
images. B mode fills FM data with `$55` and MFM data with `$AA`. The WD179x creates address-mark
clocks and CRCs from Write Track control bytes. A `$F7` control byte produces
two CRC bytes, so transfer-buffer size is not the on-disk byte count.

The final layout, starting at index, is approximately:

| Region | Encoding | Expected surviving sectors |
|--------|----------|----------------------------|
| Beginning | FM | Early FM sector numbers, approximately 1–8/9 |
| Boundary | Partial overwrite | May contain a damaged or incomplete sector |
| Remainder | MFM | Later MFM sector numbers left from the first format |

The exact split depends on timer delay, rotational timing, and interrupt
latency. **All 18 MFM sectors cannot survive** an FM overwrite of the beginning
of the track. The FM template describes a potential full write; it does not
mean all 17 FM sectors will appear on the final track.

Transfer buffers are built before installing the disk I/O handlers and caching
the disk-control register. This keeps lengthy buffer preparation outside the
I/O session while BASIC's motor-off timer may still be running.

The MFM transfer uses controller DRQ halting and masks IRQ/FIRQ during the
transfer. The FM transfer polls DRQ and leaves FIRQ available for the timer.
The active handler is `fm_dskcon_firqService()`; it changes the stacked return
address to the abort routine. Timer cleanup precedes restoration of the FIRQ
vector, including when the FM format reports an error.

## Readback and display

Each density gets 50 Read Address attempts. Only successful addresses with
matching track, side 0, expected size code, and sector number in range count.
Failed reads and unexpected IDs are reported separately as `I/O ERR` and `BAD ID`;
returned data is never used unchecked as an array index. The most recent
controller status and four ID bytes are displayed as `Sxx ID tt/ss/rr/nn`
above each density's results, including failed reads. `S10` indicates
record not found, `S08` CRC error, and `S04` lost data for Read Address;
bits may be combined. ID bytes from a failed transfer are diagnostic only.
The bottom line shows selected mode, whether the format path was entered
(`FORMAT Y/N`), and FM format status (`00` placeholder if formatting was skipped). These are sample
counts, not a guarantee that every physical sector was visited.

For each sector discovered, the program reads its data and checks every byte
against the selected pattern (256 bytes for MFM, 128 for FM). FM always uses
`$55`; MFM uses `$55` for W/older images or `$AA` for reverse images. The buffer is cleared before
reading so an incomplete transfer cannot pass using old data.

| Display | Meaning |
|---------|---------|
| COUNT `-` | No valid address observed for this sector |
| COUNT `1`–`9` | Number of valid address observations |
| COUNT `+` | Ten or more observations |
| DATA `-` | Not read because no valid address was observed |
| DATA `.` | Controller reported no error and every byte matched the selected pattern |
| DATA `E` | Controller reported an I/O error, not necessarily a CRC error |
| DATA `M` | Controller reported no error, but the payload differs from the selected pattern |

The FM format's DSKCON status is printed and retained on the results screen.
Bit 2 (`$04`) is no longer erased or hidden behind a SUCCESS message. It can
indicate lost data; following Force Interrupt its interpretation also depends
on the controller's status type (Type I uses this bit for track zero). Other
format error bits stop the operation. The saved status is DSKCON's masked
status (`$7C`), not the complete hardware status register. Readback is needed
to establish which sectors survived intact.

A boundary-sector error may be a consequence of deliberately stopping inside
a field. Tune for clean surviving sectors in both densities and, when possible,
a split in a gap. Do not interpret missing overwritten MFM sectors or unwritten
FM sectors as failures. The fixed `$55` pattern does not exercise arbitrary
payloads that resemble another encoding's address marks.

## Checking DMK persistence

Immediate write-mode results describe the in-memory emulated track. To check
the DMK writer and reader:

1. Use W mode on an expendable DMK and record the sector counts/data results,
   track number, delay, and FM format status.
2. Eject the target image so MAME saves it, then reload that DMK.
3. Choose R mode for the same track. It does not issue Write Track or Write
   Sector commands. Compare which sectors are found and their data results;
   individual observation counts can vary with rotational timing.

R mode expects the FMFM layout and the selected payload pattern; it is not a general filesystem
checker. It can also inspect an entirely FM or entirely MFM FMFM-pattern track;
address failures in the absent density are expected.

`fmfm.dsk` is the ordinary sector image used to distribute `FMFM.BIN`. It is
not a mixed-density output image; use DMK or another format that retains
per-sector encoding for the target.

## Building and verification status

Requires CMOC (with LWTOOLS) and Toolshed's `decb`. Run `make` to compile the
three C sources, link `fmfm`, create `fmfm.dsk`, and copy `FMFM.BIN` into it.

The original README reports testing under MAME 0.288; real-hardware testing
was pending. The revised program still needs functional testing, including
write/read-only comparison across DMK eject/reload, invalid input, and error
paths. No claim of identical real-hardware timing is made.

## Reverse test: FM baseline, partial MFM overwrite

Choose **B**, drive 0 (or your target drive), track 0, and initially delay 3140.
The program verifies a full 16-sector FM baseline before attempting the MFM
write. Sixteen sectors leave comfortable space on a nominal 300-RPM track;
the original 17-sector FM template plus expanded CRCs is too long for a full
track. W retains that template because it intentionally stops partway through.

The reverse result should contain early MFM sector numbers with `$AA` data and
later FM sector numbers (up to 16) with `$55` data. The exact sector split is
not fixed; delay 3140 may stop inside a field. Errors at the splice are possible,
but intact sectors on both sides should read cleanly. The top line identifies
write versus read, and the next line shows the expected MFM fill byte.
The status line labels the density of the partially written track.

The new MFM path retains DRQ/HALT-driven byte transfers and NMI completion.
Only the timed Write Track path leaves FIRQ unmasked. The GIME timer redirects
execution to a Force Interrupt abort routine; normal MFM transfers still mask
IRQ/FIRQ. The program disarms the timer and restores the original FIRQ vector
before leaving the I/O session. A format that completes without the timer
handler firing is reported as `TIMER ABORT DID NOT FIRE`, not accepted as a
successful mixed-density test. Full FM transfers use polling with timer abort
disabled, and disarm their saved NMI return vector on polled completion.

To check persistence, eject the newly written image, keep an untouched copy,
reload the copy, and choose **R**, reverse pattern **1**, the same drive and
track. Compare sector availability, payload results, and splice errors with
the immediate B-mode results. Report both image filenames for byte/CRC
inspection. R with pattern 0 remains available for earlier `$55`-only images.

After the reverse test passes, vary the delay to find a gap boundary, then
move it into a sector to exercise preservation of an intentional CRC/read
error. Re-run W as a regression check. These new paths have been compiled but
still require functional verification in MAME and on real hardware.

## Sector fill utility: SECTWR.BIN

`make` now includes a separate program, `SECTWR.BIN`, on `fmfm.dsk`. Load it
with `LOADM "SECTWR"` and run `EXEC`. `make sectwrite` builds just its executable.

Choose W (or Enter) to write and verify, R to verify without writing, or Q to
quit. Enter the drive (0–3), side (0–1), track (0–79), sector (1–255), mode (FM or MFM),
and fill value (0–255). Numbers accept decimal, `$` hexadecimal, or `0x`
hexadecimal. Empty input retains the displayed default. Side select uses the CoCo DS3
control bit with drives 0–2; drive 3 supports side 0 only because that same bit
selects drive 3. The side bit is retained on every operation and retry, and the
ID scan checks the header's side number against the selection. Select tracks
and sides supported by the attached drive and media.

The utility restores and steps to the track, scans for a matching valid ID in
the selected density, and discovers the sector size from that ID. Standard
128/256/512/1024-byte sectors are supported. It writes the selected byte to
every data byte using Write Sector, then reads the sector back, checks status,
and compares every byte. It never formats a track. Missing/invalid IDs and
unsupported size codes prevent the write. Read mode compares with the entered
expected byte, making it suitable for checking persistence after eject/reload.

For the next mixed-density test, copy `dmktest10.dmk` to a new target image.
Write track 0 MFM sector 1 with `$33` and FM sector 16 with `$CC`. Both should
report `VERIFIED`. Eject/save, keep a copy, reload it, and use R with the same
parameters to verify both patterns. Preserve the before/after images so all
untouched sectors and the existing MFM sector-10 boundary error can also be
checked. FMFM's uniform-pattern verifier will report M for the two deliberately
changed sectors; use SECTWR R to check their new values.

The utility has been compiled; sector writes and readback still need user
functional testing. No automated or emulated functional tests were run.

For side-1 verification, use a copy of a double-sided image with a valid side-1
sector. Run SECTWR W with drive 0, side 1, the target track/sector/density, and a
new byte value. Eject/save and reload, then run R with the same parameters.
Keep the before/after images to verify side 0 was not changed. A single-sided
image cannot supply the side-1 sector IDs needed for this operation.
