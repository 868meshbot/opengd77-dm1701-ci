# Network-relayed SMS: receive-path fixes (V3_TEST)

This documents a set of fixes to `application/source/functions/sms.c` (and a few
related files) in `V3_TEST/MDUV380_firmware`, made while diagnosing why SMS sent
from a real DMR network service (a status/echo-test bot on DMR ID `262993`,
reached via a WPSD hotspot) was never appearing in the inbox, and why the network
kept retransmitting the same message.

All of this was diagnosed and confirmed against real on-air captures (see
"How this was diagnosed" below), not guessed from spec documents alone.

## Symptom 1: network kept retransmitting, no ACK, nothing in inbox

WPSD/MMDVMHost logs showed the network resending the same inbound SMS every
~5 seconds, and nothing ever showed up on the radio.

**Root cause**: `smsHandleReceivedDataFrame()` only recognised inbound DMR data
headers using Data Packet Format (DPF) `0x02`/`0x03` (Confirmed/Unconfirmed
Data) -- the format this firmware's own `smsBuildDataHeader()` uses for its own
outbound sends. Real network-relayed SMS arrives as DPF `0x0D`/`0x0E` (Defined
Short/Raw Data), which lays out the "blocks to follow" field differently
(`(frame[0]&0x30)+(frame[1]&0x0F)` instead of `frame[8]&0x7F` -- see
MMDVMHost's own `DMRDataHeader.cpp`) and doesn't use the same SAP-nibble
convention. The header was silently rejected, so nothing was ever assembled or
acknowledged.

**Fix**: recognise DPF `0x0D`/`0x0E`, use the correct block-count formula for
it, skip the SAP check (it doesn't apply to this DPF), and always schedule an
ACK once such a message is fully assembled -- Defined Short Data is a
delivery-confirmed service at the protocol level regardless of whether the
payload can be decoded into readable text.

Commit: `5d6cb65 Accept DMR Defined Short/Raw Data headers for inbound SMS`

## Symptom 2: message accepted and ACKed, but garbled/missing text

Once the header was accepted, the payload still didn't decode into readable
text -- either nothing appeared, or the text appeared badly scrambled (words
spliced together from the wrong positions, e.g. "Repeater: MW0AXD" turning into
"Reter: AXD").

### How this was diagnosed

The existing decoder (`smsDecodeMotorolaPayload`/`smsDecodeStandardPayload`)
expects an IP/UDP-wrapped payload matching `DMR_Text_Message_Specification_rev_1.0.md`
in this folder -- the format this firmware's own outbound sends use. Real
network-relayed messages turned out to use a completely different, unwrapped
format, which could only be confirmed by capturing actual on-air payload bytes:

- A temporary diagnostic feature was added to `sms.c`, gated behind
  `#define SMS_DEBUG_USB_SERIAL` (0 by default -- see "Re-enabling the debug
  serial dump" below), that prints the raw TX/RX payload bytes and decode
  result over the same USB CDC serial port the radio already exposes for CPS
  programming, using this firmware's existing `USB_DEBUG_printf()`.
- The resulting hex dumps were decoded independently in Python
  (`bytes.fromhex(...).decode('utf-16-be')`) to get ground truth, completely
  separate from the firmware's own decode logic.

This showed the real payload is **plain UTF-16BE text starting at byte 0, with
no IP/UDP header and no offset at all** -- e.g. `RSSI:+40 (0)` arrives as
`00 52 00 53 00 53 00 49 ...` (each character a big-endian 16-bit code point),
immediately followed by the rest of the message.

### Bugs found and fixed

1. **`allowAscii` was a dead parameter.** `smsTryDecodeWindowDirect()` took an
   `allowAscii` flag that was passed as `true` throughout the decode pipeline,
   but the function did `(void)allowAscii;` and only ever tried UTF-16LE
   decoding. `smsDecodeAsciiPayload()` (an already-written, quality-gated ASCII
   decoder) existed but was never called from anywhere. Wired it into both the
   windowed-scan stage and a new whole-buffer fallback stage.

2. **No BE decode path was ever used.** This firmware's own outbound format is
   UTF-16LE (`smsConvertTextToUtf16LeUpper`), and `smsDecodeUtf16Payload()`
   (the "decode the whole buffer directly" function) is LE-only. A BE
   equivalent, `smsDecodeUtf16BeRun()`, existed but was marked
   `__attribute__((unused))` and never called -- same pattern as the ASCII
   decoder. It's also a "longest printable run" scanner that stops at the
   first embedded newline, which is wrong for a multi-line report. Added
   `smsDecodeUtf16BePayload()`, a proper whole-buffer BE decoder (mirrors
   `smsDecodeUtf16Payload()`, including newline handling), and made it the
   *first* thing tried in Stage 3 of `smsDecodeCurrentRxBuffers()` when no
   Motorola/DMR_Standard port or signature was recognised.

3. **A coincidental "IP packet length" match truncated the scan window.**
   `smsDecodeCurrentRxBuffers()` (and `smsShouldAckUndecodedPayload()`) both
   read `payload[2:3]` as a big-endian "IP packet length" and used it to
   shrink the decode window whenever the value happened to fall in a
   plausible range (20..payloadLength) -- with no check that this payload
   actually looks like an IP packet. For a plain BE text payload, the first
   two text characters can easily produce a value that satisfies this check
   by coincidence, silently truncating the real content. Fixed by gating that
   heuristic on `payload[0] == 0x45` (IPv4 version 4 / IHL 5), which only a
   genuine IP-wrapped payload will have.

4. **A single unmappable character reopened the door to corruption.**
   `smsTryAdoptDecodedCandidate()` treated "any `?` character in the decoded
   text" as "still poor quality, keep trying other decode strategies." A
   single unmappable character (e.g. a `°` degree sign with no ASCII mapping)
   in an otherwise long, correct decode was enough to let the
   offset-scanning/windowed-neighborhood fallback stages run afterward, whose
   results then got spliced into the already-correct text
   character-by-character via `smsMergeDecodedCandidate()` -- silently
   dropping/overwriting correct substrings with fragments from a completely
   different (wrong) alignment. This is what turned "Swansea" into "Ssea" and
   scrambled the rest of a message after its first line. Replaced the "any `?`
   at all" check with `smsDecodedTextIsPoor()`, a proportional threshold (more
   than ~20% of the text unrecognised) shared by every decode stage.

Verified against multiple real captures (`rssi` and `wx swansea, wales`
queries to DMR ID `262993`) by decoding the complete raw bytes independently
in Python and confirming an exact match with the firmware's decoded output.

**This conclusion turned out to be wrong, and was corrected in Symptom 3
below.** The above only proved that *decoding* the firmware's own already-
assembled bytes was faithful to those bytes -- it did not prove that
*reassembling* those bytes from individual DMR blocks in the first place
was correct. It wasn't: see below.

## Symptom 3: garbled text was actually caused by our own block reassembly, not the sender

The wording noted above ("Reter" instead of "Repeater", "T26299eportat"
instead of "TG262993 Report at") was wrongly attributed to a bug in
`262993`'s own message composition. It is not. Enabling verbose MMDVMHost
logging (`DisplayLevel=1`/`FileLevel=1` under `[Log]` in `MMDVM.ini` --
level `1` is the *most* verbose, not least) exposes `CUtils::dump()` hex
dumps of every DMR data header and data block MMDVMHost itself receives,
independent of this firmware entirely. Reconstructing a "WX Report -
Swansea" message from those block-level dumps (stripping what turned out to
be a 2-byte per-block header, then decoding the rest as UTF-16BE) produced a
perfectly clean, complete report -- proving the sender was never buggy.

**Root cause**: `smsHandleReceivedDataFrame()` treated every inbound DMR
data block as a fixed `SMS_BLOCK_DATA_BYTES` (12) bytes, whether or not it
also stripped a 2-byte header for `dataType == 0x08`. Real rate-3/4 DMR data
bursts (`dataType 0x08`, used for Confirmed Data and Defined Short Data --
MMDVMHost labels these "Data 3/4" in its log) actually carry **16 bytes** of
payload per block (18 bytes total), not 10. The firmware was under-reading
every rate-3/4 block by 6 bytes, silently truncating/misaligning the
reassembled message -- for a 10-block message that's 60 bytes lost out of
160, exactly matching the garbling and length shortfall observed.

Confirmed by cross-referencing our own firmware's parsed DPF/pad/block-count
against MMDVMHost's independent parse of the *same* frame byte-for-byte
(matched exactly across four separate captures) before concluding the block
*count* logic was correct and the block *size* assumption was the actual
bug.

**Fix**: the 12-byte read from the HR-C6000 chip
(`SPI0ReadPageRegByteArray(0x02, 0x00, dataSyncBuf, LC_DATA_LENGTH)` in
`HR-C6000.c`) was hardcoded regardless of burst type. Since `rxDataType` is
already known before that read happens, it now reads
`SMS_RATE34_DATA_LENGTH` (18) bytes specifically for `rxDataType == 0x08`,
and passes the actual length read through to `smsHandleReceivedDataFrame()`
(which gained a `frameLength` parameter) so the per-block payload size is
derived from real data instead of an assumed constant. The RX reassembly
buffers (`smsRxPayloadBuffer`/`smsRxDecodePendingPayloadBuffer`) were grown
accordingly. Verified: `wx swansea, wales` now decodes to the complete,
correct four-line report.

This was inherently a leap of faith about the HR-C6000 chip's own register
behaviour (no datasheet access to confirm reading 18 bytes at that SPI
address yields real data rather than garbage past byte 12) -- it was only
confirmed correct by trying it on real hardware and checking the result.

## Symptom 4: decoded text duplicated (e.g. "ADGJMPTW" -> "ADGJMPTWADGJMPTW")

After the symptom 1-3 fixes above, real-world testing against BrandMeister
TextCapture turned up a new bug: decoded SMS text was sometimes duplicated,
e.g. sending `adgjmptw` produced `adgjmptwadgjmptw` in the inbox, and sending
`MW0AXD` produced `MW0AXDW0AXDAXD`. This was diagnosed entirely from real CDC
serial captures with `SMS_DEBUG_USB_SERIAL` re-enabled (see below), across
several passes as each fix revealed the next layer of the bug.

**Root cause (general shape):** `smsDecodeCurrentRxBuffers()` runs several
decode strategies in sequence -- a direct windowed read at the known
Motorola/Standard text offset, a "neighborhood" scan around that offset, a
scan of a UDP-payload offset, and whole-buffer fallback scans -- each gated
on `smsDecodedTextIsPoor(decodedText)` so that a later, worse strategy
shouldn't run once a good decode already exists. Several of these gates were
missing or incomplete, so a correct decode from an earlier stage kept getting
re-scanned by a later stage, and the (garbage or offset-shifted) result got
spliced onto the already-correct text via `smsMergeDecodedCandidate()`'s
character-by-character extension merge -- producing the tail of the same
message appended a second time, at a shifted alignment.

Three separate gaps were found and fixed, each confirmed against a distinct
real capture:

1. **Motorola-window neighborhood scan** ran unconditionally after the
   direct-window decode, even when that direct decode had already succeeded.
   Fixed by wrapping it in `if (smsDecodedTextIsPoor(decodedText))`.
   Confirmed via a capture showing `"MW0AXD"` decode correctly once, then
   get re-scanned and merged into `"MW0AXDW0AXDAXD"`.

2. **Standard-window guard was ad hoc and incomplete.** The prior code used
   `bool allowStandardWindow = true;` plus a narrow Motorola-only check,
   which didn't cover the case where Stage 1 (`smsDecodeStandardPayload`)
   had already produced a good decode. Replaced with
   `bool allowStandardWindow = smsDecodedTextIsPoor(decodedText);`, a single
   correct guard covering both cases.

3. **UDP-payload-offset window was completely unguarded.** This block ran
   whenever a Motorola or Standard port was recognised and
   `udpPayloadLength > 0`, regardless of whether an earlier stage had
   already decoded the message correctly -- and because this offset starts
   *before* the real text offset, it re-included header bytes as leading
   garbage followed by the same real text again. Confirmed via a capture of
   a sent `"wxy"` test message coming back as `"a\n\n                    WXY"`.
   Fixed by adding `&& smsDecodedTextIsPoor(decodedText)` to the outer `if`
   that guards the whole block, not just the neighborhood-scan fallback
   inside it.

## Symptom 5: short (2-character) messages like "CK" still duplicated after the symptom-4 fixes

Even with all three gates above in place, a 2-character message ("CK", used
as a diagnostic during the BrandMeister ACK investigation) still came back
duplicated, while 3+ character messages ("DMW", "TEST AXD", "123") decoded
cleanly. The difference: `smsScoreDecodedText()` unconditionally rejects any
candidate shorter than 3 characters (returns its minimum/worst score), as a
guard against short garbage from the whole-buffer fallback scans. That
guard, applied uniformly, also rejected short-but-*correct* primary-offset
reads -- so a correct 2-character decode was never "trusted", `decodedText`
stayed poor, and every later fallback stage still ran and corrupted it.

**Fix:** added a dedicated read of the primary offset (Motorola text offset
if a Motorola signature/port was recognised, else the Standard text offset)
directly into `decodedText` *before* the existing Stage 2 logic runs, bypassing
`smsScoreDecodedText()`'s length gate entirely for this one read -- it's the
mathematically correct offset for this message, not a speculative scan, so
the usual noise-rejection reasoning doesn't apply to it. Later stages still
only run if this primary read fails to produce anything
(`smsDecodedTextIsPoor` still gates them).

Note: this fix is logically sound and code-reviewed against the same
duplication mechanism as symptom 4, but the specific live BrandMeister
traffic that would have exercised a short message stopped (server-side)
before a fresh confirming capture could be taken. Treat it as
provisionally-fixed pending one more real 2-character message test.

## Re-enabling the debug serial dump

If you need to capture raw SMS payload bytes again in the future (e.g. to
diagnose another network service's format), flip the flag near the top of
`sms.c`:

```c
#define SMS_DEBUG_USB_SERIAL 1
```

This prints, over the same USB CDC serial port used for CPS programming
(no JTAG/SWD probe needed):

- `SMS TX to=... from=... text="..."` + a hex dump, whenever a message is sent.
- `SMS RX from=... totalLength=... padOctets=...` + a hex dump, for every
  inbound SMS payload, before any decode attempt runs.
- `SMS RX decode OK/FAILED: "..."`, showing what (if anything) got decoded.

To read it, open the radio's CPS-mode serial device with a terminal. Watch
out for two capture pitfalls hit while diagnosing this:

- Piping through a plain `cat /dev/tty... > file` on macOS fully-buffers
  output once redirected to a file, so nothing appears until the buffer fills
  or the process exits -- use `script -q file cat /dev/tty...` instead
  (`stdbuf` isn't available on macOS), or read the device directly with
  `dd if=/dev/cu.usbmodemXXXX of=file bs=1` for byte-at-a-time writes.
- Long lines can appear truncated in a terminal simply due to window width;
  capture to a file and read the file rather than trusting what scrolled by
  on screen.

## Other fixes from the same review pass (not SMS-decode-specific)

A separate full-tree code review of `V3_TEST/MDUV380_firmware/application`
(functions/, hardware/drivers, and the UI layer) found and fixed six
critical memory-safety issues unrelated to SMS decoding -- unbounded USB/CPS
memory reads/writes, a stack buffer overflow in the hotspot IP-info handler,
two buffer overflows in message-box display code, and a divide-by-zero in
DMR-ID cache initialization. See commit `b69eab1` for details.
