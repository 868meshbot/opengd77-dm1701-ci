# SMS send-side: format choice, SAP validation, and a known TX limitation (V3_TEST)

## "Send as" format choice

The compose flow (`menuSMS.c`) now shows a "Send as" menu step right after the
destination is resolved (contact, manual ID, or reply-to-sender), before the
message actually transmits:

```
Send as
> Motorola
  DMR_Standard
```

This threads a `smsEncoderFormat_t` (`SMS_ENCODER_MOTOROLA` /
`SMS_ENCODER_STANDARD`) through the whole TX pipeline --
`smsPackMessage()` -> `smsQueueMessage()` ->
`smsScheduleQueuedMessageTransmission()` -> the outgoing-tracking structs
(so a retry re-sends in the same format originally chosen). Resending a
message from the *Sent* list always uses Motorola, since that storage
doesn't currently record which format a message was originally sent in.

`smsBuildStandardPayload()`/`smsBuildStandardUdpHeader()` (new) implement
the "DMR_Standard" format -- UDP port `0x1398` (5016), a 4-byte internal
sub-header (`0x00 0x0D 0x00 0x0A`) instead of Motorola's 10 bytes, text at
offset 32 instead of 38. Built to match `smsDecodeStandardPayload()`
byte-for-byte (that decoder already worked against real captures), rather
than re-deriving from the spec PDF, which turned out to have some
ambiguous/garbled table formatting for this section.

**Important naming caveat**: despite the name, this "DMR_Standard" format is
**not** the same thing as the actual ETSI-standard "Defined Short Data"
format real network services and (per a real report) Anytone radios use --
see below. The name comes from the original spec document bundled with this
repo, which uses "DMR_Standard" for this specific IP/UDP-wrapped, CRC32-
terminated format. Confusingly, that's a different, apparently older/
alternate convention from the actual ETSI `Defined Short Data` PDU format
this session spent most of its time getting *receive* support for. Verified
correct on the wire (byte-for-byte: UDP port, sub-header constants, UDP
length math) but not yet confirmed against a real Anytone radio's screen.

## SAP validation for Defined Short/Raw Data headers

Per a report of the real spec (ETSI TS 102 361-1 clause 9.2.12, Defined Data
Short Data packet Header (DD_HEAD) PDU): SAP identifier is `0b1010` (0xA),
DPF is `0b1101` (0x0D, matching what was already implemented), and there's
no block CRC32. `smsHandleReceivedDataFrame()` previously skipped SAP
validation entirely for this DPF (since the correct expected value wasn't
known); it now requires `frame[1] & 0xF0 == 0xA0`. Verified against the two
real Defined Short Data headers already captured this session (`4D AA ...`
and `4D A8 ...` -- both have SAP nibble `0xA0`), so this tightening doesn't
regress anything already confirmed working.

## Known limitation: no TX support for rate-3/4 blocks (blocks a true Anytone/Defined-Short-Data *encoder*)

Real Defined Short Data messages (RSSI/WX reports from network services,
and reportedly real Anytone radios) use "rate 3/4" DMR data bursts --
16 bytes of payload per block, not the 12 this firmware's rate-1/2 format
uses (this is what `SMS_RATE34_DATA_LENGTH` in `HR-C6000.h` fixed for
*receiving*). Building a matching *encoder* was attempted and stopped
partway for two independent reasons:

1. **Header CRC / bytes 8-9 unknown.** The two real Defined Short Data
   headers captured this session (`4D AA 23 F6 6E 04 03 51 53 20 D8 75` and
   `4D A8 23 F6 6E 04 03 51 53 00 3A 70`) don't decode cleanly under any
   common CRC16 parameterization (poly/init/reflection/XOR-out) tried
   against both samples simultaneously, and bytes 8-9 (`53 20` / `53 00`)
   don't have a confirmed meaning. Getting this wrong wouldn't just garble
   text -- since DMR bursts are CRC-checked in hardware before any
   application code runs, a wrong header CRC means the receiving radio's
   chip silently discards the whole burst, so the feature would appear to
   work (TX debug shows "keyed up") while never actually being received by
   anything.

2. **TX hardware path has no rate-3/4 support at all.** `HR-C6000.c`'s SMS
   TX frame handling (`hrc.smsFrames[SMS_MAX_TX_FRAMES][LC_DATA_LENGTH]`,
   `hrc6000SendSMSFrame()`, `hrc6000GetSmsDataType()`) is hardcoded to only
   ever send rate-1/2 (12-byte, `dataType 0x07`) blocks -- there's no code
   path that produces `dataType 0x08`. Even with a perfect header/payload,
   the hardware transmit path would still chop it into the wrong block
   structure. Extending this would need the frame buffer resized/made
   variable, the SPI write length changed, and (unverified, no chip
   datasheet access) figuring out whether/how the HR-C6000 needs additional
   register configuration to actually encode a rate-3/4 burst on transmit.

Given both blockers, this was intentionally stopped rather than shipping a
best-effort/unverified encoder. If picked up again: get more real Defined
Short Data header captures (different block counts/content) to have enough
data points to reverse-engineer the CRC and bytes 8-9 with confidence, and
separately investigate the HR-C6000 TX-side rate-3/4 question (ideally with
real chip register documentation, not just black-box capture-based
reverse-engineering as used for the receive side).
