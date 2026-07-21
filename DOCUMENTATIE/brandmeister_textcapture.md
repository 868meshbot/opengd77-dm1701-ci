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
