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
The remaining "odd" wording in some reports (e.g. "Reter" instead of
"Repeater", "T26299eportat" instead of "TG262993 Report at") is confirmed to
be present in the sender's own raw bytes -- i.e. a bug/quirk in that network
service's own message composition, not something this firmware's receive path
can or should try to correct.

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
