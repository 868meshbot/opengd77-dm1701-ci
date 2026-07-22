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

#include <string.h>
#include "functions/csbk.h"
#include "functions/sms.h"
#include "functions/ticks.h"
#include "functions/trx.h"
#include "hardware/HR-C6000.h"
#include "usb/usb_com.h"

// Diagnostic logging used to root-cause the Call Alert / Radio Check TX-hang investigation
// (root cause: menuDataGlobal.data[] array-length mismatch in menuSystem.c, now fixed).
// Left in place, off by default, mirroring sms.c's SMS_DEBUG_USB_SERIAL pattern.
#define CSBK_DEBUG_USB_SERIAL 0

#define CSBK_LC_DATA_LENGTH   12U
#define CSBK_REPEAT_COUNT      3U
#define CSBK_ACK_TIMEOUT_MS 2000U

typedef struct
{
	bool queued;
	csbkKind_t kind;
	uint32_t destinationId;
	uint32_t sourceId;
	uint8_t frame[CSBK_LC_DATA_LENGTH];
} csbkQueuedRequest_t;

typedef struct
{
	csbkPendingState_t state;
	csbkKind_t kind;
	uint32_t destinationId;
	ticksTimer_t timeoutTimer;
} csbkPendingOutbound_t;

static csbkQueuedRequest_t queuedRequest = { 0 };
static csbkPendingOutbound_t pendingOutbound = { 0 };

static uint16_t csbkCrc16Ccitt(const uint8_t *data, uint8_t length)
{
	uint16_t crc = 0x0000U;

	for (uint8_t index = 0U; index < length; index++)
	{
		crc ^= ((uint16_t)data[index] << 8);
		for (uint8_t bit = 0U; bit < 8U; bit++)
		{
			if ((crc & 0x8000U) != 0U)
			{
				crc = (uint16_t)((crc << 1) ^ 0x1021U);
			}
			else
			{
				crc <<= 1;
			}
		}
	}

	return crc;
}

// Builds a complete, self-contained standalone CSBK frame (opcode + FID + service byte +
// dest/source addresses + CRC). Unlike sms.c's Preamble CSBK, this is the whole PDU -- nothing
// else is transmitted after it besides repeats of itself, see csbkOnly in smsPreparedMessage_t.
static void csbkBuildFrame(uint8_t *frame, csbkOpcode_t opcode, uint8_t serviceByte, uint32_t destinationId, uint32_t sourceId)
{
	uint16_t crc;

	memset(frame, 0, CSBK_LC_DATA_LENGTH);
	frame[0] = (uint8_t)(0x80U | (opcode & 0x3FU)); // Last Block=1, Protect Flag=0, CSBKO
	frame[1] = 0x00U;                                // FID=0 (standard, non-vendor)
	frame[2] = serviceByte;
	frame[3] = 0x00U;
	frame[4] = (uint8_t)((destinationId >> 16) & 0xFFU);
	frame[5] = (uint8_t)((destinationId >> 8) & 0xFFU);
	frame[6] = (uint8_t)(destinationId & 0xFFU);
	frame[7] = (uint8_t)((sourceId >> 16) & 0xFFU);
	frame[8] = (uint8_t)((sourceId >> 8) & 0xFFU);
	frame[9] = (uint8_t)(sourceId & 0xFFU);

	crc = csbkCrc16Ccitt(frame, 10U);
	frame[10] = (uint8_t)((crc >> 8) & 0xFFU) ^ 0xA5U;
	frame[11] = (uint8_t)(crc & 0xFFU) ^ 0xA5U;
}

static bool csbkIdValid(uint32_t id)
{
	return ((id != 0U) && (id <= 0x00FFFFFFU));
}

void csbkInit(void)
{
	memset(&queuedRequest, 0, sizeof(queuedRequest));
	memset(&pendingOutbound, 0, sizeof(pendingOutbound));
}

static bool csbkStartRequest(csbkKind_t kind, csbkOpcode_t opcode, uint32_t destinationId, uint32_t sourceId)
{
	if (!csbkIdValid(destinationId) || !csbkIdValid(sourceId) ||
		queuedRequest.queued || (pendingOutbound.state == CSBK_PENDING_WAITING) ||
		smsHasQueuedMessage() || HRC6000IsSendingSMS())
	{
#if CSBK_DEBUG_USB_SERIAL
		USB_DEBUG_printf("CSBK startRequest REJECTED kind=%d opcode=0x%02x dest=%lu src=%lu queued=%d pendingWaiting=%d smsQueued=%d smsSending=%d\r\n",
			(int)kind, (unsigned)opcode, (unsigned long)destinationId, (unsigned long)sourceId,
			(int)queuedRequest.queued, (int)(pendingOutbound.state == CSBK_PENDING_WAITING),
			(int)smsHasQueuedMessage(), (int)HRC6000IsSendingSMS());
#endif
		return false;
	}

	queuedRequest.kind = kind;
	queuedRequest.destinationId = destinationId;
	queuedRequest.sourceId = sourceId;
	csbkBuildFrame(queuedRequest.frame, opcode, 0x00U, destinationId, sourceId);
	queuedRequest.queued = true;

#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK startRequest OK kind=%d opcode=0x%02x dest=%lu src=%lu\r\n",
		(int)kind, (unsigned)opcode, (unsigned long)destinationId, (unsigned long)sourceId);
#endif

	return true;
}

bool csbkSendCallAlert(uint32_t destinationId, uint32_t sourceId)
{
	return csbkStartRequest(CSBK_KIND_CALL_ALERT, CSBKO_CALL_ALERT, destinationId, sourceId);
}

bool csbkSendRadioCheck(uint32_t destinationId, uint32_t sourceId)
{
	return csbkStartRequest(CSBK_KIND_RADIO_CHECK, CSBKO_RADIO_CHECK_REQ, destinationId, sourceId);
}

csbkPendingState_t csbkGetPendingResult(uint32_t *sourceIdOut, csbkKind_t *kindOut)
{
	csbkPendingState_t result = pendingOutbound.state;

	if ((result == CSBK_PENDING_ACKED) || (result == CSBK_PENDING_TIMEOUT))
	{
		if (sourceIdOut != NULL)
		{
			*sourceIdOut = pendingOutbound.destinationId;
		}

		if (kindOut != NULL)
		{
			*kindOut = pendingOutbound.kind;
		}

		pendingOutbound.state = CSBK_PENDING_NONE;
	}

	return result;
}

void csbkHandleReceivedFrame(const uint8_t *buf, uint8_t length)
{
	uint16_t crc;
	uint16_t receivedCrc;
	uint8_t opcode;
	uint32_t destId;
	uint32_t srcId;

	if ((buf == NULL) || (length < CSBK_LC_DATA_LENGTH))
	{
		return;
	}

	crc = csbkCrc16Ccitt(buf, 10U);
	receivedCrc = (uint16_t)(((uint16_t)(buf[10] ^ 0xA5U) << 8) | (buf[11] ^ 0xA5U));

	if (crc != receivedCrc)
	{
		return;
	}

	opcode = (uint8_t)(buf[0] & 0x3FU);
	destId = (((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 8) | buf[6]);
	srcId  = (((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 8) | buf[9]);

	if ((destId != trxDMRID) || (trxDMRID == 0U))
	{
		return;
	}

	switch (opcode)
	{
		case CSBKO_CALL_ALERT:
		case CSBKO_RADIO_CHECK_REQ:
			// Silent for Radio Check (no UI, no sound -- that's what makes it a "check" rather than
			// an alert); Call Alert notification/tone is left to the UI layer to add on top of this
			// once this has been confirmed against real hardware -- the protocol-correct auto-Ack
			// is the part that matters for interop and is implemented here unconditionally.
			(void)csbkStartRequest(CSBK_KIND_CALL_ALERT, CSBKO_ACK, srcId, trxDMRID);
			if (queuedRequest.queued)
			{
				// Tag the queued Ack's service byte so the far end can tell which request it answers.
				queuedRequest.frame[2] = opcode;
				{
					uint16_t crcAck = csbkCrc16Ccitt(queuedRequest.frame, 10U);
					queuedRequest.frame[10] = (uint8_t)((crcAck >> 8) & 0xFFU) ^ 0xA5U;
					queuedRequest.frame[11] = (uint8_t)(crcAck & 0xFFU) ^ 0xA5U;
				}
			}
			break;

		case CSBKO_ACK:
			if ((pendingOutbound.state == CSBK_PENDING_WAITING) && (pendingOutbound.destinationId == srcId))
			{
				uint8_t ackedService = buf[2];

				if (((ackedService == CSBKO_CALL_ALERT) && (pendingOutbound.kind == CSBK_KIND_CALL_ALERT)) ||
					((ackedService == CSBKO_RADIO_CHECK_REQ) && (pendingOutbound.kind == CSBK_KIND_RADIO_CHECK)))
				{
					pendingOutbound.state = CSBK_PENDING_ACKED;
				}
			}
			break;

		default:
			// Unrecognised CSBKO -- per spec, compliant equipment ignores these. Do the same.
			break;
	}
}

void csbkTick(void)
{
	if (queuedRequest.queued &&
		(trxDMRID != 0U) &&
		(slotState == DMR_STATE_IDLE) &&
		!smsHasQueuedMessage() &&
		!HRC6000IsSendingSMS() &&
		!HRC6000IRQHandlerIsRunning())
	{
#if CSBK_DEBUG_USB_SERIAL
		USB_DEBUG_printf("CSBK tick: attempting TX start, frame[0]=0x%02x dest=%lu\r\n",
			queuedRequest.frame[0], (unsigned long)queuedRequest.destinationId);
#endif

		if (smsQueueRawCsbkMessage(queuedRequest.frame, CSBK_REPEAT_COUNT))
		{
#if CSBK_DEBUG_USB_SERIAL
			USB_DEBUG_printf("CSBK tick: queued into sms.c OK, calling HRC6000StartQueuedSMS\r\n");
#endif

			if (HRC6000StartQueuedSMS())
			{
#if CSBK_DEBUG_USB_SERIAL
				USB_DEBUG_printf("CSBK tick: HRC6000StartQueuedSMS returned true (TX started)\r\n");
#endif

				// Only the two request opcodes (not our own Ack replies) get outbound-result tracking.
				if ((queuedRequest.frame[0] & 0x3FU) != CSBKO_ACK)
				{
					pendingOutbound.state = CSBK_PENDING_WAITING;
					pendingOutbound.kind = queuedRequest.kind;
					pendingOutbound.destinationId = queuedRequest.destinationId;
					ticksTimerStart(&pendingOutbound.timeoutTimer, CSBK_ACK_TIMEOUT_MS);
				}

				queuedRequest.queued = false;
			}
			else
			{
#if CSBK_DEBUG_USB_SERIAL
				USB_DEBUG_printf("CSBK tick: HRC6000StartQueuedSMS returned false, clearing queued message\r\n");
#endif
				smsClearQueuedMessage();
			}
		}
#if CSBK_DEBUG_USB_SERIAL
		else
		{
			USB_DEBUG_printf("CSBK tick: smsQueueRawCsbkMessage FAILED (message already queued?)\r\n");
		}
#endif
	}

	if ((pendingOutbound.state == CSBK_PENDING_WAITING) && ticksTimerHasExpired(&pendingOutbound.timeoutTimer))
	{
#if CSBK_DEBUG_USB_SERIAL
		USB_DEBUG_printf("CSBK tick: pending outbound request timed out (no Ack)\r\n");
#endif
		pendingOutbound.state = CSBK_PENDING_TIMEOUT;
	}
}
