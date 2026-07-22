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
#include <stdlib.h>
#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/menuCsbkActions.h"
#include "functions/csbk.h"
#include "functions/sms.h"
#include "functions/sound.h"
#include "functions/trx.h"
#include "functions/codeplug.h"
#include "usb/usb_com.h"

// Diagnostic logging used to root-cause the Call Alert / Radio Check TX-hang investigation
// (root cause: menuDataGlobal.data[] array-length mismatch in menuSystem.c, now fixed).
// Left in place, off by default, mirroring sms.c's SMS_DEBUG_USB_SERIAL pattern.
#define CSBK_DEBUG_USB_SERIAL 0

// Minimal, self-contained screen for the three new DMR services: Call Alert, Radio Check and
// Status. Deliberately reuses none of menuSMSCompose's internal state machine (kept fully
// separate to avoid any risk of touching that already-tested code) -- it's a plain numeric
// DMR ID entry (mirroring smsComposeManualIdBuffer's key handling in menuSMS.c) plus, for
// Status, a short fixed code list beforehand. Results (Ack/timeout/status-received) are
// reported later via the global notification poll in applicationMain.c, exactly like normal
// SMS TX events already are -- this screen doesn't wait around for them.

typedef enum
{
	CSBK_ACTION_MODE_STATUS_SELECT = 0,
	CSBK_ACTION_MODE_DESTINATION_SELECT,
	CSBK_ACTION_MODE_CONTACT_SELECT,
	CSBK_ACTION_MODE_ID_ENTRY
} csbkActionMode_t;

#define CSBK_DESTINATION_OPTION_COUNT 2U

typedef struct
{
	uint8_t code;
	const char *label;
} csbkStatusEntry_t;

static const csbkStatusEntry_t csbkStatusTable[] = {
	{ 1U, "Available" },
	{ 2U, "En Route" },
	{ 3U, "On Scene" },
	{ 4U, "Busy" },
	{ 5U, "Returning" },
	{ 6U, "Out of Svc" },
	{ 7U, "Need Help" },
	{ 8U, "Testing" },
};
#define CSBK_STATUS_TABLE_COUNT (sizeof(csbkStatusTable) / sizeof(csbkStatusTable[0]))

static csbkActionKind_t csbkActionKind = CSBK_ACTION_CALL_ALERT;
static csbkActionMode_t csbkActionMode = CSBK_ACTION_MODE_ID_ENTRY;
static char csbkActionIdBuffer[11] = { 0 };
static uint8_t csbkActionSelectedStatusIndex = 0U;
static uint8_t csbkActionDestOptionIndex = 0U;
static uint16_t csbkActionContactIndex = 0U;

static void csbkActionsRenderStatusSelect(void);
static void csbkActionsRenderDestinationSelect(void);
static void csbkActionsRenderContactSelect(void);
static void csbkActionsRenderIdEntry(void);
static void csbkActionsTrySend(void);
static bool csbkActionsGetPrivateContactAt(uint16_t listIndex, CodeplugContact_t *contact);

void menuCsbkActionsSetKind(csbkActionKind_t kind)
{
#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: menuCsbkActionsSetKind(%d)\r\n", (int)kind);
#endif
	csbkActionKind = kind;
}

static const char *csbkActionsTitle(void)
{
	switch (csbkActionKind)
	{
		case CSBK_ACTION_CALL_ALERT:
			return "Call Alert";
		case CSBK_ACTION_RADIO_CHECK:
			return "Radio Check";
		case CSBK_ACTION_STATUS:
		default:
			return "Send Status";
	}
}

static void csbkActionsRenderStatusSelect(void)
{
	int mNum;

	displayClearBuf();
	menuDisplayTitle(csbkActionsTitle());

	for (int i = MENU_START_ITERATION_VALUE; i < MENU_END_ITERATION_VALUE; i++)
	{
		mNum = menuGetMenuOffset(CSBK_STATUS_TABLE_COUNT, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY)
		{
			continue;
		}
		else if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)
		{
			break;
		}
		menuDisplayEntry(i, mNum, csbkStatusTable[mNum].label, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_COLOUR_NONE, THEME_ITEM_BG);
	}

	displayRender();
}

static void csbkActionsRenderDestinationSelect(void)
{
	const char *options[CSBK_DESTINATION_OPTION_COUNT] = { "Select contact", "Manual ID" };
	int mNum;

	menuDataGlobal.numItems = CSBK_DESTINATION_OPTION_COUNT;
	if (csbkActionDestOptionIndex >= CSBK_DESTINATION_OPTION_COUNT)
	{
		csbkActionDestOptionIndex = 0U;
	}
	menuDataGlobal.currentItemIndex = csbkActionDestOptionIndex;

	displayClearBuf();
	menuDisplayTitle(csbkActionsTitle());

	for (int i = MENU_START_ITERATION_VALUE; i < MENU_END_ITERATION_VALUE; i++)
	{
		mNum = menuGetMenuOffset(CSBK_DESTINATION_OPTION_COUNT, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY)
		{
			continue;
		}
		else if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)
		{
			break;
		}
		menuDisplayEntry(i, mNum, options[mNum], 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_COLOUR_NONE, THEME_ITEM_BG);
	}

	displayRender();
}

static bool csbkActionsGetPrivateContactAt(uint16_t listIndex, CodeplugContact_t *contact)
{
	return (codeplugContactGetDataForNumberInType((int)(listIndex + 1U), CONTACT_CALLTYPE_PC, contact) > 0);
}

static void csbkActionsRenderContactSelect(void)
{
	char line[SCREEN_LINE_BUFFER_SIZE];
	int count = codeplugContactsGetCount(CONTACT_CALLTYPE_PC);

	displayClearBuf();
	menuDisplayTitle("Select contact");

	if (count <= 0)
	{
		displayThemeApply(THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_BG);
		displayPrintAt(DISPLAY_X_POS_MENU_TEXT_OFFSET, DISPLAY_Y_POS_MENU_START + FONT_SIZE_2_HEIGHT, "No private contacts", FONT_SIZE_2);
		displayThemeApply(THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
		displayPrintAt(DISPLAY_X_POS_MENU_TEXT_OFFSET, DISPLAY_SIZE_Y - FONT_SIZE_1_HEIGHT, "Red back", FONT_SIZE_1);
		displayThemeResetToDefault();
		displayRender();
		return;
	}

	for (int i = MENU_START_ITERATION_VALUE; i < MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(count, i);
		CodeplugContact_t contact;
		char contactName[17] = { 0 };

		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY)
		{
			continue;
		}
		else if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)
		{
			break;
		}

		if (csbkActionsGetPrivateContactAt((uint16_t)mNum, &contact))
		{
			codeplugUtilConvertBufToString(contact.name, contactName, 16);
			if (contactName[0] == 0)
			{
				strncpy(contactName, "(no name)", sizeof(contactName));
				contactName[sizeof(contactName) - 1] = 0;
			}
			snprintf(line, sizeof(line), "%s", contactName);
		}
		else
		{
			strncpy(line, "Invalid", sizeof(line));
			line[sizeof(line) - 1] = 0;
		}

		menuDisplayEntry(i, mNum, line, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_COLOUR_NONE, THEME_ITEM_BG);
	}

	displayThemeApply(THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	displayPrintAt(DISPLAY_X_POS_MENU_TEXT_OFFSET, DISPLAY_SIZE_Y - FONT_SIZE_1_HEIGHT, "Green select  Red back", FONT_SIZE_1);
	displayThemeResetToDefault();
	displayRender();
}

static void csbkActionsRenderIdEntry(void)
{
	char line[SCREEN_LINE_BUFFER_SIZE];

#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: renderIdEntry enter\r\n");
#endif

	if (csbkActionIdBuffer[0] == 0)
	{
		strncpy(line, "ID:", sizeof(line));
		line[sizeof(line) - 1] = 0;
	}
	else
	{
		snprintf(line, sizeof(line), "ID:%s", csbkActionIdBuffer);
	}

#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: renderIdEntry line built, calling displayClearBuf\r\n");
#endif
	displayClearBuf();
#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: renderIdEntry displayClearBuf OK, calling menuDisplayTitle\r\n");
#endif
	menuDisplayTitle(csbkActionsTitle());
#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: renderIdEntry menuDisplayTitle OK\r\n");
#endif
	displayThemeApply(THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_BG);
	displayPrintAt(DISPLAY_X_POS_MENU_TEXT_OFFSET, DISPLAY_Y_POS_MENU_START + FONT_SIZE_2_HEIGHT, line, FONT_SIZE_2);
	displayThemeApply(THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	displayPrintAt(DISPLAY_X_POS_MENU_TEXT_OFFSET, DISPLAY_SIZE_Y - FONT_SIZE_1_HEIGHT, "Green send  Red back", FONT_SIZE_1);
	displayThemeResetToDefault();
#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: renderIdEntry about to call displayRender\r\n");
#endif
	displayRender();
#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: renderIdEntry displayRender OK, function complete\r\n");
#endif
}

static void csbkActionsTrySend(void)
{
	char *endPtr = NULL;
	unsigned long parsedId;
	bool started = false;

	if (csbkActionIdBuffer[0] == 0)
	{
		soundSetMelody(MELODY_ERROR_BEEP);
		return;
	}

	parsedId = strtoul(csbkActionIdBuffer, &endPtr, 10);
	if ((endPtr == NULL) || (*endPtr != 0) || (parsedId == 0UL) || (parsedId > 0x00FFFFFFUL))
	{
		soundSetMelody(MELODY_ERROR_BEEP);
		return;
	}

	if (trxGetMode() != RADIO_MODE_DIGITAL)
	{
		uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_USER, 2000U, "DMR only", true);
		return;
	}

#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: trySend kind=%d id=%lu\r\n", (int)csbkActionKind, parsedId);
#endif

	switch (csbkActionKind)
	{
		case CSBK_ACTION_CALL_ALERT:
			started = csbkSendCallAlert((uint32_t)parsedId, trxDMRID);
			break;

		case CSBK_ACTION_RADIO_CHECK:
			started = csbkSendRadioCheck((uint32_t)parsedId, trxDMRID);
			break;

		case CSBK_ACTION_STATUS:
		default:
			if (smsQueueStatusMessage((uint32_t)parsedId, trxDMRID, csbkStatusTable[csbkActionSelectedStatusIndex].code) == SMS_PACK_OK)
			{
				started = smsScheduleQueuedStatusTransmission((uint32_t)parsedId, trxDMRID);
				if (!started)
				{
					smsClearQueuedMessage();
				}
			}
			break;
	}

#if CSBK_DEBUG_USB_SERIAL
	USB_DEBUG_printf("CSBK UI: trySend result started=%d\r\n", (int)started);
#endif

	if (started)
	{
		uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_USER, 2000U, "Sending...", true);
		menuSystemPopPreviousMenu();
	}
	else
	{
		soundSetMelody(MELODY_NACK_BEEP);
		uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_USER, 2000U, "Busy, try again", true);
	}
}

menuStatus_t menuCsbkActions(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		csbkActionIdBuffer[0] = 0;
		csbkActionSelectedStatusIndex = 0U;
		csbkActionDestOptionIndex = 0U;
		csbkActionContactIndex = 0U;
		csbkActionMode = (csbkActionKind == CSBK_ACTION_STATUS) ? CSBK_ACTION_MODE_STATUS_SELECT : CSBK_ACTION_MODE_DESTINATION_SELECT;

		if (csbkActionMode == CSBK_ACTION_MODE_STATUS_SELECT)
		{
			menuDataGlobal.currentItemIndex = 0;
			menuDataGlobal.numItems = CSBK_STATUS_TABLE_COUNT;
			csbkActionsRenderStatusSelect();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}

		menuDataGlobal.currentItemIndex = 0;
		menuDataGlobal.numItems = CSBK_DESTINATION_OPTION_COUNT;
		csbkActionsRenderDestinationSelect();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	if ((ev->events & FUNCTION_EVENT) && (ev->function == FUNC_REDRAW))
	{
		switch (csbkActionMode)
		{
			case CSBK_ACTION_MODE_STATUS_SELECT:
				csbkActionsRenderStatusSelect();
				break;
			case CSBK_ACTION_MODE_DESTINATION_SELECT:
				csbkActionsRenderDestinationSelect();
				break;
			case CSBK_ACTION_MODE_CONTACT_SELECT:
				csbkActionsRenderContactSelect();
				break;
			case CSBK_ACTION_MODE_ID_ENTRY:
			default:
				csbkActionsRenderIdEntry();
				break;
		}
		return (MENU_STATUS_SUCCESS);
	}

	if (csbkActionMode == CSBK_ACTION_MODE_STATUS_SELECT)
	{
		if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
		{
			menuSystemPopPreviousMenu();
			return (MENU_STATUS_SUCCESS);
		}

		if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
		{
			menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, CSBK_STATUS_TABLE_COUNT);
			csbkActionsRenderStatusSelect();
		}
		else if (KEYCHECK_PRESS(ev->keys, KEY_UP))
		{
			menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, CSBK_STATUS_TABLE_COUNT);
			csbkActionsRenderStatusSelect();
		}
		else if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			csbkActionSelectedStatusIndex = (uint8_t)menuDataGlobal.currentItemIndex;
			csbkActionMode = CSBK_ACTION_MODE_DESTINATION_SELECT;
			menuDataGlobal.currentItemIndex = 0;
			menuDataGlobal.numItems = CSBK_DESTINATION_OPTION_COUNT;
			csbkActionsRenderDestinationSelect();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}

		return (MENU_STATUS_SUCCESS);
	}

	if (csbkActionMode == CSBK_ACTION_MODE_DESTINATION_SELECT)
	{
		if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
		{
			if (csbkActionKind == CSBK_ACTION_STATUS)
			{
				csbkActionMode = CSBK_ACTION_MODE_STATUS_SELECT;
				menuDataGlobal.currentItemIndex = csbkActionSelectedStatusIndex;
				menuDataGlobal.numItems = CSBK_STATUS_TABLE_COUNT;
				csbkActionsRenderStatusSelect();
				return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
			}

			menuSystemPopPreviousMenu();
			return (MENU_STATUS_SUCCESS);
		}

		if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
		{
			menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, CSBK_DESTINATION_OPTION_COUNT);
			csbkActionDestOptionIndex = (uint8_t)menuDataGlobal.currentItemIndex;
			csbkActionsRenderDestinationSelect();
		}
		else if (KEYCHECK_PRESS(ev->keys, KEY_UP))
		{
			menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, CSBK_DESTINATION_OPTION_COUNT);
			csbkActionDestOptionIndex = (uint8_t)menuDataGlobal.currentItemIndex;
			csbkActionsRenderDestinationSelect();
		}
		else if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			if (csbkActionDestOptionIndex == 0U)
			{
				int privateContactsCount = codeplugContactsGetCount(CONTACT_CALLTYPE_PC);

				if (privateContactsCount <= 0)
				{
					soundSetMelody(MELODY_ERROR_BEEP);
					uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_USER, 1500U, "No contacts", true);
					return (MENU_STATUS_SUCCESS);
				}

				csbkActionContactIndex = 0U;
				csbkActionMode = CSBK_ACTION_MODE_CONTACT_SELECT;
				menuDataGlobal.currentItemIndex = 0;
				menuDataGlobal.numItems = privateContactsCount;
				csbkActionsRenderContactSelect();
				return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
			}

			csbkActionMode = CSBK_ACTION_MODE_ID_ENTRY;
			csbkActionIdBuffer[0] = 0;
			csbkActionsRenderIdEntry();
			return (MENU_STATUS_INPUT_TYPE | MENU_STATUS_SUCCESS);
		}

		return (MENU_STATUS_SUCCESS);
	}

	if (csbkActionMode == CSBK_ACTION_MODE_CONTACT_SELECT)
	{
		int privateContactsCount = codeplugContactsGetCount(CONTACT_CALLTYPE_PC);

		if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
		{
			csbkActionMode = CSBK_ACTION_MODE_DESTINATION_SELECT;
			menuDataGlobal.currentItemIndex = csbkActionDestOptionIndex;
			menuDataGlobal.numItems = CSBK_DESTINATION_OPTION_COUNT;
			csbkActionsRenderDestinationSelect();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}

		if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
		{
			menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, privateContactsCount);
			csbkActionContactIndex = (uint16_t)menuDataGlobal.currentItemIndex;
			csbkActionsRenderContactSelect();
		}
		else if (KEYCHECK_PRESS(ev->keys, KEY_UP))
		{
			menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, privateContactsCount);
			csbkActionContactIndex = (uint16_t)menuDataGlobal.currentItemIndex;
			csbkActionsRenderContactSelect();
		}
		else if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			CodeplugContact_t selectedContact;

			if (csbkActionsGetPrivateContactAt(csbkActionContactIndex, &selectedContact) == false)
			{
				soundSetMelody(MELODY_ERROR_BEEP);
				uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_USER, 1500U, "Invalid contact", true);
				return (MENU_STATUS_SUCCESS);
			}

			snprintf(csbkActionIdBuffer, sizeof(csbkActionIdBuffer), "%lu", (unsigned long)selectedContact.tgNumber);
			csbkActionsTrySend();
		}

		return (MENU_STATUS_SUCCESS);
	}

	// CSBK_ACTION_MODE_ID_ENTRY
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		csbkActionMode = CSBK_ACTION_MODE_DESTINATION_SELECT;
		menuDataGlobal.currentItemIndex = csbkActionDestOptionIndex;
		menuDataGlobal.numItems = CSBK_DESTINATION_OPTION_COUNT;
		csbkActionsRenderDestinationSelect();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT) || KEYCHECK_SHORTUP(ev->keys, KEY_HASH))
	{
		size_t len = strlen(csbkActionIdBuffer);
		if (len > 0U)
		{
			csbkActionIdBuffer[len - 1U] = 0;
		}
		csbkActionsRenderIdEntry();
		return (MENU_STATUS_SUCCESS);
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		csbkActionsTrySend();
		return (MENU_STATUS_SUCCESS);
	}

	char digit = 0;
	if (KEYCHECK_SHORTUP(ev->keys, KEY_0)) digit = '0';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_1)) digit = '1';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_2)) digit = '2';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_3)) digit = '3';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_4)) digit = '4';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_5)) digit = '5';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_6)) digit = '6';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_7)) digit = '7';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_8)) digit = '8';
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_9)) digit = '9';

	if (digit != 0)
	{
		size_t len = strlen(csbkActionIdBuffer);
		if (len < (sizeof(csbkActionIdBuffer) - 1U))
		{
			csbkActionIdBuffer[len] = digit;
			csbkActionIdBuffer[len + 1U] = 0;
			csbkActionsRenderIdEntry();
		}
		else
		{
			soundSetMelody(MELODY_ERROR_BEEP);
		}
	}

	return (MENU_STATUS_SUCCESS);
}
