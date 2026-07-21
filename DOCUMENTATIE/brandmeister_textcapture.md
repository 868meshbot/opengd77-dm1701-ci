# BrandMeister TextCapture and this firmware's ACK behavior

BrandMeister has a store-and-forward SMS feature called TextCapture
(<https://news.brandmeister.network/new-textcapture-feature-the-sms-store-and-forward-service-you-can-now-enable-in-your-self-care/>).
This page explains, in plain terms, what that feature requires from the radio
and confirms whether this firmware meets it.

## What TextCapture requires

From BrandMeister's own description: when enabled, TextCapture intercepts SMS
sent to your DMR ID and holds it if your radio is offline. When your radio
reappears, it delivers the message and **waits for your radio to acknowledge
receipt**. If no ACK arrives, BrandMeister keeps resending the same message
"on a regular basis" for up to 7 days before giving up.

In other words: **if your radio doesn't ACK, you get spammed with the same
message for a week.** BrandMeister's own writeup explicitly warns about this
and tells users to confirm their radio supports SMS acknowledgment before
turning TextCapture on.

## Does this firmware ACK correctly?

Yes, as of `5d6cb65` (see
[`sms_network_relay_decode_fixes.md`](sms_network_relay_decode_fixes.md) for
the full technical writeup). Before that fix, the answer was **no** — this is
the exact bug that commit fixed, discovered from the "network keeps
retransmitting, radio never ACKs" symptom on a live BrandMeister-relayed test
message.

The relevant detail: BrandMeister/TextCapture relays SMS using the DMR
"Defined Short/Raw Data" header format (DPF `0x0D`/`0x0E`), not the format
this firmware uses for its own radio-to-radio sends. This firmware now:

1. Recognises that header format (previously it silently rejected it).
2. Sends an ACK **once the message is fully assembled, unconditionally** —
   not only if the text happens to decode cleanly. Defined Short Data is a
   delivery-confirmed service at the protocol level, so the radio ACKs it the
   same way a real handset would, even in the rare case the text itself can't
   be read back out. This is deliberate: an unACKed message means
   BrandMeister keeps resending it for a week, which is worse than
   occasionally failing to decode one message's text.

See `rxAssembly.alwaysAck` / `rxDecodePending.alwaysAck` in `sms.c` for the
exact logic.

## Which radios have this fix

| Tree | Has the ACK fix | Notes |
|---|---|---|
| `V3_TEST` (DM1701) | Yes | Confirmed on real hardware against a live network test service. |
| `MD9600_RT90` | Yes | Ported and Docker-compiled; **not yet tested on real hardware.** |
| `V2_STM32-MOB` | Yes | Ported and Docker-compiled; **not yet tested on real hardware.** |
| `V2_DEBUG` | **No, on purpose** | This is a passive receive/capture build (`SMS_RX_DEBUG_DISABLE_ACK=1`) meant for sniffing traffic during development, not for actual use. Don't enable TextCapture against a radio running this build — it will never ACK and BrandMeister will keep resending for a week. |

## Practical takeaway

If you're running `V3_TEST`/DM1701, TextCapture should work as designed. If
you're on `MD9600_RT90` or `V2_STM32-MOB`, the fix is present and builds
clean, but hasn't been proven on real hardware yet — treat it as
provisionally working, and it'd be worth one real test message before relying
on it. Don't enable TextCapture on a `V2_DEBUG` build at all.

## Known remaining issue: BrandMeister can still redeliver despite a correct ACK

Real hardware testing after the ACK fix above showed the radio sending a
correct low-level Response PDU ACK (`smsQueueAckResponseMessage()`, DPF
`0x01`, ETSI Transport ACK) for every message it receives, yet BrandMeister's
TextCapture sometimes kept redelivering the same message anyway.

An automatic reply of the literal text `"CK"` was tried as a workaround, on
the theory that BrandMeister's TextCapture specifically wants an
application-level "Motorola-style" SMS ACK rather than (or in addition to)
the ETSI transport ACK. **This made things worse, not better, and was fully
reverted.** With auto-reply enabled, BrandMeister relayed the radio's own
`"CK"` reply back to it as if it were a brand new incoming message (source
and destination DMR ID are identical for a TextCapture self-reply), which the
radio then correctly received and ACKed — triggering another reply, another
redelivery, and so on. This was confirmed from real captured logs showing the
radio's own reply content looping back as "new" traffic, not guessed from
symptoms alone.

**Current state:** no auto-reply code is present in any tree. The ETSI
Transport ACK from `smsQueueAckResponseMessage()` is the only ACK this
firmware sends, and it fires correctly and unconditionally for every
recognised inbound message. Whether BrandMeister's redelivery behavior is
fully resolved by that ACK alone, by a BrandMeister self-care protocol
setting (ETSI vs. Motorola vs. "Chinese radio" — changing this setting
changes the wire format BrandMeister uses, and was observed to change/reduce
the redelivery pattern in testing), or requires something else entirely,
**remains unresolved and is not a firmware-side bug to keep chasing.** If you
see repeated redelivery, treat it as a BrandMeister/hotspot-side issue first:
check the self-care TextCapture/SMS protocol setting, and if it persists,
raise it with BrandMeister/hotspot support rather than the radio.
