/* Spa Bluez5 ModemManager proxy
 *
 * Copyright © 2022 Collabora
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef SPA_BLUEZ5_MODEMMANAGER_H_
#define SPA_BLUEZ5_MODEMMANAGER_H_

#include <spa/utils/list.h>

#include "defs.h"

enum cmee_error {
	CMEE_AG_FAILURE = 0,
	CMEE_NO_CONNECTION_TO_PHONE = 1,
	CMEE_OPERATION_NOT_ALLOWED = 3,
	CMEE_OPERATION_NOT_SUPPORTED = 4,
	CMEE_INVALID_CHARACTERS_TEXT_STRING = 25,
	CMEE_INVALID_CHARACTERS_DIAL_STRING = 27,
	CMEE_NO_NETWORK_SERVICE = 30
};

enum call_setup {
	CIND_CALLSETUP_NONE = 0,
	CIND_CALLSETUP_INCOMING,
	CIND_CALLSETUP_DIALING,
	CIND_CALLSETUP_ALERTING
};

enum call_direction {
	CALL_OUTGOING,
	CALL_INCOMING
};

enum call_state {
	CLCC_ACTIVE,
	CLCC_HELD,
	CLCC_DIALING,
	CLCC_ALERTING,
	CLCC_INCOMING,
	CLCC_WAITING,
	CLCC_RESPONSE_AND_HOLD
};

struct call {
	struct spa_list link;
	unsigned int index;
	struct impl *this;
	DBusPendingCall *pending;

	char *path;
	char *number;
	bool call_indicator;
	enum call_direction direction;
	enum call_state state;
	bool multiparty;
};

struct mm_ops {
	void (*send_cmd_result)(bool success, enum cmee_error error, void *user_data);
	void (*set_modem_service)(bool available, void *user_data);
	void (*set_modem_signal_strength)(unsigned int strength, void *user_data);
	void (*set_modem_operator_name)(const char *name, void *user_data);
	void (*set_modem_own_number)(const char *number, void *user_data);
	void (*set_modem_roaming)(bool is_roaming, void *user_data);
	void (*set_call_active)(bool active, void *user_data);
	void (*set_call_setup)(enum call_setup value, void *user_data);
};

#ifdef HAVE_BLUEZ_5_BACKEND_NATIVE_MM
void *mm_register(struct spa_log *log, void *dbus_connection, const struct spa_dict *info,
                  const struct mm_ops *ops, void *user_data);
void mm_unregister(void *data);
bool mm_is_available(void *modemmanager);
unsigned int mm_supported_features();
bool mm_answer_call(void *modemmanager, void *user_data, enum cmee_error *error);
bool mm_hangup_call(void *modemmanager, void *user_data, enum cmee_error *error);
bool mm_do_call(void *modemmanager, const char* number, void *user_data, enum cmee_error *error);
bool mm_send_dtmf(void *modemmanager, const char *dtmf, void *user_data, enum cmee_error *error);
const char *mm_get_incoming_call_number(void *modemmanager);
struct spa_list *mm_get_calls(void *modemmanager);
#else
void *mm_register(struct spa_log *log, void *dbus_connection, const struct spa_dict *info,
                  const struct mm_ops *ops, void *user_data)
{
	return NULL;
}

void mm_unregister(void *data)
{
}

bool mm_is_available(void *modemmanager)
{
	return false;
}

unsigned int mm_supported_features(void)
{
	return 0;
}

bool mm_answer_call(void *modemmanager, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

bool mm_hangup_call(void *modemmanager, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

bool mm_do_call(void *modemmanager, const char* number, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

bool mm_send_dtmf(void *modemmanager, const char *dtmf, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

const char *mm_get_incoming_call_number(void *modemmanager)
{
	return NULL;
}

struct spa_list *mm_get_calls(void *modemmanager)
{
	return NULL;
}
#endif

#endif
