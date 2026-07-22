/*
 * Copyright (C) 2026
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _OPENGD77_CSBK_H_
#define _OPENGD77_CSBK_H_

#include <stdint.h>
#include <stdbool.h>

// Standalone (non-preamble) CSBK services: Call Alert (page a radio, it notifies its user) and
// Radio Check (silently ask if a radio is on/in range; it auto-replies with no user interaction).
//
// IMPORTANT -- opcode values below are this author's best recollection of the ETSI TS 102 361-1
// CSBKO table, NOT independently verified against this repo's own tested code the way the SMS
// Preamble CSBK opcode (0x3D, in sms.c's smsBuildCsbk()) was -- that one is proven correct because
// the whole SMS send path was confirmed against real BrandMeister/MMDVMHost traffic this session.
// These are not. Compliant DMR equipment is specified to silently discard CSBKO values it doesn't
// recognise, so a wrong guess here is expected to just mean "doesn't interoperate with a real
// AnyTone/Hytera yet", not something more disruptive -- but treat these as unverified until
// checked against a real capture (e.g. MMDVMHost verbose DMR log of a genuine Call Alert / Radio
// Check from another radio), same as every other wire-format fact in this codebase was confirmed.
typedef enum
{
	CSBKO_CALL_ALERT      = 0x1FU, // UNVERIFIED -- request: page destination, it shows/plays a notification.
	CSBKO_RADIO_CHECK_REQ = 0x1DU, // UNVERIFIED -- request: destination silently auto-replies, no UI.
	CSBKO_ACK             = 0x20U  // UNVERIFIED -- response to either of the above (byte[2] carries which).
} csbkOpcode_t;

typedef enum
{
	CSBK_KIND_CALL_ALERT = 0,
	CSBK_KIND_RADIO_CHECK
} csbkKind_t;

typedef enum
{
	CSBK_PENDING_NONE = 0,
	CSBK_PENDING_WAITING,
	CSBK_PENDING_ACKED,
	CSBK_PENDING_TIMEOUT
} csbkPendingState_t;

void csbkInit(void);

// Send a Call Alert / Radio Check request to destinationId. Returns false if one is already
// in flight (only one outbound request is tracked at a time) or destinationId is invalid.
bool csbkSendCallAlert(uint32_t destinationId, uint32_t sourceId);
bool csbkSendRadioCheck(uint32_t destinationId, uint32_t sourceId);

// Poll the outcome of the most recent csbkSendCallAlert()/csbkSendRadioCheck(). Returns
// CSBK_PENDING_NONE once there is nothing left to report (after a terminal state has been read
// once, it resets to NONE on the next call).
csbkPendingState_t csbkGetPendingResult(uint32_t *sourceIdOut, csbkKind_t *kindOut);

// Called from HR-C6000.c's RX dispatch when a standalone CSBK burst (not a preamble) is received.
void csbkHandleReceivedFrame(const uint8_t *buf, uint8_t length);

// Called from the same place smsTick() is, to start a queued request once the radio is idle and
// to time out a request that never got an Ack.
void csbkTick(void);

#endif
