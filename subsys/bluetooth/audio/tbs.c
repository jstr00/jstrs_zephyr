/* Bluetooth TBS - Telephone Bearer Service
 *
 * Copyright (c) 2020 Bose Corporation
 * Copyright (c) 2021-2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/audio/ccid.h>
#include <zephyr/bluetooth/audio/tbs.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/check.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/types.h>

#include "audio_internal.h"
#include "tbs_internal.h"

LOG_MODULE_REGISTER(bt_tbs, CONFIG_BT_TBS_LOG_LEVEL);

#define BT_TBS_VALID_STATUS_FLAGS(val) ((val) <= (BIT(0) | BIT(1)))

/* A service instance can either be a GTBS or a TBS instance */
struct tbs_inst {
	/* Attribute values */
	char provider_name[CONFIG_BT_TBS_MAX_PROVIDER_NAME_LENGTH];
	char uci[BT_TBS_MAX_UCI_SIZE];
	uint8_t technology;
	uint8_t signal_strength;
	uint8_t signal_strength_interval;
	uint8_t ccid;
	uint16_t optional_opcodes;
	uint16_t status_flags;
	struct bt_tbs_in_uri incoming_uri;
	struct bt_tbs_in_uri friendly_name;
	struct bt_tbs_in_uri in_call;
	char uri_scheme_list[CONFIG_BT_TBS_MAX_SCHEME_LIST_LENGTH];
	struct bt_tbs_terminate_reason terminate_reason;
	struct bt_tbs_call calls[CONFIG_BT_TBS_MAX_CALLS];

	bool notify_current_calls;
	bool notify_call_states;
	bool pending_signal_strength_notification;
	struct k_work_delayable reporting_interval_work;

	/** Service Attributes */
	const struct bt_gatt_attr *attrs;
	/** Service Attribute count */
	size_t attr_count;

	bool authorization_required;
};

static struct tbs_inst svc_insts[CONFIG_BT_TBS_BEARER_COUNT];
static struct tbs_inst gtbs_inst;

#define READ_BUF_SIZE                                                                              \
	MAX(BT_ATT_MAX_ATTRIBUTE_LEN,                                                              \
	    (CONFIG_BT_TBS_MAX_CALLS * sizeof(struct bt_tbs_current_call_item) *                   \
	     (1U + ARRAY_SIZE(svc_insts))))
NET_BUF_SIMPLE_DEFINE_STATIC(read_buf, READ_BUF_SIZE);

/* Used to notify app with held calls in case of join */
static struct bt_tbs_call *held_calls[CONFIG_BT_TBS_MAX_CALLS];
static uint8_t held_calls_cnt;

static struct bt_tbs_cb *tbs_cbs;

static bool inst_is_registered(const struct tbs_inst *inst)
{
	return inst->attrs != NULL;
}

static bool inst_is_gtbs(const struct tbs_inst *inst)
{
	if (CONFIG_BT_TBS_BEARER_COUNT > 0) {
		return inst == &gtbs_inst;
	} else {
		return true;
	}
}

static uint8_t inst_index(const struct tbs_inst *inst)
{
	ptrdiff_t index = 0;

	__ASSERT_NO_MSG(inst);

	if (inst_is_gtbs(inst)) {
		return BT_TBS_GTBS_INDEX;
	}

	index = inst - svc_insts;
	__ASSERT(index >= 0 && index < ARRAY_SIZE(svc_insts), "Invalid tbs_inst pointer");

	return (uint8_t)index;
}

static struct tbs_inst *inst_lookup_index(uint8_t index)
{
	struct tbs_inst *inst = NULL;

	if (index == BT_TBS_GTBS_INDEX) {
		inst = &gtbs_inst;
	} else if (ARRAY_SIZE(svc_insts) > 0U && index < ARRAY_SIZE(svc_insts)) {
		inst = &svc_insts[index];
	}

	if (inst == NULL || !inst_is_registered(inst)) {
		return NULL;
	}

	return inst;
}

static struct bt_tbs_call *lookup_call_in_inst(struct tbs_inst *inst, uint8_t call_index)
{
	if (call_index == BT_TBS_FREE_CALL_INDEX) {
		return NULL;
	}

	for (int i = 0; i < ARRAY_SIZE(svc_insts[i].calls); i++) {
		if (inst->calls[i].index == call_index) {
			return &inst->calls[i];
		}
	}

	return NULL;
}

/**
 * @brief Finds and returns a call
 *
 * @param call_index The ID of the call
 * @return struct bt_tbs_call* Pointer to the call. NULL if not found
 */
static struct bt_tbs_call *lookup_call(uint8_t call_index)
{
	struct bt_tbs_call *call;

	if (call_index == BT_TBS_FREE_CALL_INDEX) {
		return NULL;
	}

	call = lookup_call_in_inst(&gtbs_inst, call_index);
	if (call != NULL) {
		return call;
	}

	for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
		call = lookup_call_in_inst(&svc_insts[i], call_index);
		if (call != NULL) {
			return call;
		}
	}

	return NULL;
}

static bool inst_check_attr(struct tbs_inst *inst, const struct bt_gatt_attr *attr)
{
	for (size_t j = 0; j < inst->attr_count; j++) {
		if (&inst->attrs[j] == attr) {
			return true;
		}
	}

	return false;
}

static struct tbs_inst *lookup_inst_by_attr(const struct bt_gatt_attr *attr)
{
	if (attr == NULL) {
		return NULL;
	}

	for (int i = 0; i < ARRAY_SIZE(svc_insts); i++) {
		if (inst_check_attr(&svc_insts[i], attr)) {
			return &svc_insts[i];
		}
	}

	if (inst_check_attr(&gtbs_inst, attr)) {
		return &gtbs_inst;
	}

	return NULL;
}

static struct tbs_inst *lookup_inst_by_call_index(uint8_t call_index)
{
	if (call_index == BT_TBS_FREE_CALL_INDEX) {
		return NULL;
	}

	if (lookup_call_in_inst(&gtbs_inst, call_index) != NULL) {
		return &gtbs_inst;
	}

	for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
		if (lookup_call_in_inst(&svc_insts[i], call_index) != NULL) {
			return &svc_insts[i];
		}
	}

	return NULL;
}

static bool is_authorized(const struct tbs_inst *inst, struct bt_conn *conn)
{
	if (inst->authorization_required) {
		if (tbs_cbs != NULL && tbs_cbs->authorize != NULL) {
			return tbs_cbs->authorize(conn);
		} else {
			return false;
		}
	}

	return true;
}

static bool uri_scheme_in_list(const char *uri_scheme, const char *uri_scheme_list)
{
	const size_t scheme_len = strlen(uri_scheme);
	const size_t scheme_list_len = strlen(uri_scheme_list);
	const char *uri_scheme_cand = uri_scheme_list;
	size_t uri_scheme_cand_len;
	size_t start_idx = 0;

	for (size_t i = 0; i < scheme_list_len; i++) {
		if (uri_scheme_list[i] == ',') {
			uri_scheme_cand_len = i - start_idx;
			if (uri_scheme_cand_len != scheme_len) {
				continue;
			}

			if (memcmp(uri_scheme, uri_scheme_cand, scheme_len) == 0) {
				return true;
			}

			if (i + 1 < scheme_list_len) {
				uri_scheme_cand = &uri_scheme_list[i + 1];
			}
		}
	}

	return false;
}

static struct tbs_inst *lookup_inst_by_uri_scheme(const uint8_t *uri, uint8_t uri_len)
{
	char uri_scheme[CONFIG_BT_TBS_MAX_URI_LENGTH] = {0};

	if (uri_len == 0) {
		return NULL;
	}

	/* Look for ':' between the first and last char */
	for (uint8_t i = 1U; i < uri_len - 1U; i++) {
		if (uri[i] == ':') {
			(void)memcpy(uri_scheme, uri, i);
			break;
		}
	}

	if (uri_scheme[0] == '\0') {
		/* No URI scheme found */
		return NULL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(svc_insts[i].calls); j++) {
			if (uri_scheme_in_list(uri_scheme, svc_insts[i].uri_scheme_list)) {
				return &svc_insts[i];
			}
		}
	}

	/* If not found in any TBS instance, check GTBS */
	if (uri_scheme_in_list(uri_scheme, gtbs_inst.uri_scheme_list)) {
		return &gtbs_inst;
	}

	return NULL;
}

static void tbs_set_terminate_reason(struct tbs_inst *inst, uint8_t call_index, uint8_t reason)
{
	inst->terminate_reason.call_index = call_index;
	inst->terminate_reason.reason = reason;
	LOG_DBG("Index %u: call index 0x%02x, reason %s", inst_index(inst), call_index,
		bt_tbs_term_reason_str(reason));

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_TERMINATE_REASON, inst->attrs,
			    (void *)&inst->terminate_reason, sizeof(inst->terminate_reason));
}

/**
 * @brief Gets the next free call_index
 *
 * For each new call, the call index should be incremented and wrap at 255.
 * However, the index = 0 is reserved for outgoing calls
 *
 * Call indexes are shared among all bearers, so there is always a 1:1 between a call index and a
 * bearer
 *
 * @return uint8_t The next free call index
 */
static uint8_t next_free_call_index(void)
{
	for (int i = 0; i < CONFIG_BT_TBS_MAX_CALLS; i++) {
		static uint8_t next_call_index;
		const struct bt_tbs_call *call;

		/* For each new call, the call index should be incremented */
		next_call_index++;

		if (next_call_index == BT_TBS_FREE_CALL_INDEX) {
			/* call_index = 0 reserved for outgoing calls */
			next_call_index = 1;
		}

		call = lookup_call(next_call_index);
		if (call == NULL) {
			return next_call_index;
		}
	}

	LOG_DBG("No more free call spots");

	return BT_TBS_FREE_CALL_INDEX;
}

static struct bt_tbs_call *call_alloc(struct tbs_inst *inst, uint8_t state, const uint8_t *uri,
				      uint16_t uri_len)
{
	struct bt_tbs_call *free_call = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(inst->calls); i++) {
		if (inst->calls[i].index == BT_TBS_FREE_CALL_INDEX) {
			free_call = &inst->calls[i];
			break;
		}
	}

	if (free_call == NULL) {
		return NULL;
	}

	__ASSERT_NO_MSG(uri_len < sizeof(free_call->remote_uri));

	memset(free_call, 0, sizeof(*free_call));

	/* Get the next free call_index */
	free_call->index = next_free_call_index();
	__ASSERT_NO_MSG(free_call->index != BT_TBS_FREE_CALL_INDEX);

	free_call->state = state;
	(void)memcpy(free_call->remote_uri, uri, uri_len);
	free_call->remote_uri[uri_len] = '\0';

	return free_call;
}

static void call_free(struct bt_tbs_call *call)
{
	call->index = BT_TBS_FREE_CALL_INDEX;
}

static void net_buf_put_call_states_by_inst(const struct tbs_inst *inst, struct net_buf_simple *buf)
{
	const struct bt_tbs_call *call;
	const struct bt_tbs_call *calls;
	size_t call_count;

	calls = inst->calls;
	call_count = ARRAY_SIZE(inst->calls);

	for (size_t i = 0; i < call_count; i++) {
		call = &calls[i];
		if (call->index == BT_TBS_FREE_CALL_INDEX) {
			continue;
		}

		if (buf->len + 3U > buf->size) {
			LOG_WRN("Not able to store all call states in buffer");
			return;
		}

		net_buf_simple_add_u8(buf, call->index);
		net_buf_simple_add_u8(buf, call->state);
		net_buf_simple_add_u8(buf, call->flags);
	}
}

static void net_buf_put_call_states(const struct tbs_inst *inst, struct net_buf_simple *buf)
{
	net_buf_simple_reset(buf);

	net_buf_put_call_states_by_inst(inst, buf);

	/* For GTBS we add all the calls the GTBS bearer has itself, as well as all the other
	 * bearers
	 */
	if (inst_is_gtbs(inst)) {
		for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
			net_buf_put_call_states_by_inst(&svc_insts[i], buf);
		}
	}
}

static void net_buf_put_current_calls_by_inst(const struct tbs_inst *inst,
					      struct net_buf_simple *buf)
{
	const struct bt_tbs_call *call;
	const struct bt_tbs_call *calls;
	size_t call_count;
	size_t uri_length;
	size_t item_len;

	calls = inst->calls;
	call_count = ARRAY_SIZE(inst->calls);

	for (size_t i = 0; i < call_count; i++) {
		call = &calls[i];
		if (call->index == BT_TBS_FREE_CALL_INDEX) {
			continue;
		}

		uri_length = strlen(call->remote_uri);
		item_len = sizeof(call->index) + sizeof(call->state) + sizeof(call->flags) +
			   uri_length;

		__ASSERT_NO_MSG(item_len <= UINT8_MAX);

		if (buf->len + sizeof(uint8_t) + item_len > buf->size) {
			LOG_WRN("Not able to store all calls in buffer");
			return;
		}

		net_buf_simple_add_u8(buf, (uint8_t)item_len);
		net_buf_simple_add_u8(buf, call->index);
		net_buf_simple_add_u8(buf, call->state);
		net_buf_simple_add_u8(buf, call->flags);
		net_buf_simple_add_mem(buf, call->remote_uri, uri_length);
	}
}

static void net_buf_put_current_calls(const struct tbs_inst *inst, struct net_buf_simple *buf)
{
	net_buf_simple_reset(buf);

	net_buf_put_current_calls_by_inst(inst, buf);

	/* For GTBS we add all the calls the GTBS bearer has itself, as well as all the other
	 * bearers
	 */
	if (inst_is_gtbs(inst)) {
		for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
			net_buf_put_current_calls_by_inst(&svc_insts[i], buf);
		}
	}
}

static int inst_notify_calls(const struct tbs_inst *inst)
{
	int err;

	if (inst->notify_call_states) {
		net_buf_put_call_states(inst, &read_buf);

		err = bt_gatt_notify_uuid(NULL, BT_UUID_TBS_CALL_STATE, inst->attrs, read_buf.data,
					  read_buf.len);
		if (err != 0) {
			return err;
		}
	}

	if (inst->notify_current_calls) {
		net_buf_put_current_calls(inst, &read_buf);

		err = bt_gatt_notify_uuid(NULL, BT_UUID_TBS_LIST_CURRENT_CALLS, inst->attrs,
					  read_buf.data, read_buf.len);
		if (err != 0) {
			return err;
		}
	}

	return 0;
}

static int notify_calls(const struct tbs_inst *inst)
{
	int err;

	if (inst == NULL) {
		return -EINVAL;
	}

	/* Notify TBS */
	err = inst_notify_calls(inst);
	if (err != 0) {
		return err;
	}

	if (!inst_is_gtbs(inst)) {
		/* If the instance is different than the GTBS notify on the GTBS instance as well */
		err = inst_notify_calls(&gtbs_inst);
		if (err != 0) {
			return err;
		}
	}

	return 0;
}

static ssize_t read_provider_name(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				  uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u, Provider name %s", inst_index(inst), inst->provider_name);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, inst->provider_name,
				 strlen(inst->provider_name));
}

static void provider_name_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_uci(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u: UCI %s", inst_index(inst), inst->uci);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, inst->uci, strlen(inst->uci));
}

static ssize_t read_technology(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u: Technology 0x%02x", inst_index(inst), inst->technology);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &inst->technology,
				 sizeof(inst->technology));
}

static void technology_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_uri_scheme_list(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				    void *buf, uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	net_buf_simple_reset(&read_buf);

	net_buf_simple_add_mem(&read_buf, inst->uri_scheme_list, strlen(inst->uri_scheme_list));

	if (inst_is_gtbs(inst)) {
		/* TODO: Make uri schemes unique */
		for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
			size_t uri_len = strlen(svc_insts[i].uri_scheme_list);

			if (read_buf.len + uri_len >= read_buf.size) {
				LOG_WRN("Cannot fit all TBS instances in GTBS "
					"URI scheme list");
				break;
			}

			net_buf_simple_add_mem(&read_buf, svc_insts[i].uri_scheme_list, uri_len);
		}

		LOG_DBG("GTBS: URI scheme %.*s", read_buf.len, read_buf.data);
	} else {
		LOG_DBG("Index %u: URI scheme %.*s", inst_index(inst), read_buf.len, read_buf.data);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, read_buf.data, read_buf.len);
}

static void uri_scheme_list_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_signal_strength(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				    void *buf, uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u: Signal strength 0x%02x", inst_index(inst), inst->signal_strength);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &inst->signal_strength,
				 sizeof(inst->signal_strength));
}

static void signal_strength_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_signal_strength_interval(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					     void *buf, uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u: Signal strength interval 0x%02x",
		inst_index(inst), inst->signal_strength_interval);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &inst->signal_strength_interval,
				 sizeof(inst->signal_strength_interval));
}

static ssize_t write_signal_strength_interval(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					      const void *buf, uint16_t len, uint16_t offset,
					      uint8_t flags)
{
	struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);
	struct net_buf_simple net_buf;
	uint8_t signal_strength_interval;

	if (!is_authorized(inst, conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(signal_strength_interval)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	net_buf_simple_init_with_data(&net_buf, (void *)buf, len);
	signal_strength_interval = net_buf_simple_pull_u8(&net_buf);

	inst->signal_strength_interval = signal_strength_interval;
	LOG_DBG("Index %u: 0x%02x", inst_index(inst), signal_strength_interval);

	return len;
}

static void current_calls_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
		inst->notify_current_calls = (value == BT_GATT_CCC_NOTIFY);
	}
}

static ssize_t read_current_calls(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				  uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u", inst_index(inst));

	net_buf_put_current_calls(inst, &read_buf);

	if (offset == 0) {
		LOG_HEXDUMP_DBG(read_buf.data, read_buf.len, "Current calls");
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, read_buf.data, read_buf.len);
}

static ssize_t read_ccid(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u: CCID 0x%02x", inst_index(inst), inst->ccid);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &inst->ccid, sizeof(inst->ccid));
}

static ssize_t read_status_flags(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				 uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);
	const uint16_t status_flags_le = sys_cpu_to_le16(inst->optional_opcodes);

	LOG_DBG("Index %u: status_flags 0x%04x", inst_index(inst), inst->status_flags);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &status_flags_le,
				 sizeof(status_flags_le));
}

static void status_flags_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_incoming_uri(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				 uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);
	const struct bt_tbs_in_uri *inc_call_target;
	size_t val_len;

	inc_call_target = &inst->incoming_uri;

	LOG_DBG("Index %u: call index 0x%02x, URI %s", inst_index(inst),
		inc_call_target->call_index, inc_call_target->uri);

	if (!inc_call_target->call_index) {
		LOG_DBG("URI not set");

		return bt_gatt_attr_read(conn, attr, buf, len, offset, NULL, 0);
	}

	val_len = sizeof(inc_call_target->call_index) + strlen(inc_call_target->uri);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, inc_call_target, val_len);
}

static void incoming_uri_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_call_state(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);

	LOG_DBG("Index %u", inst_index(inst));

	net_buf_put_call_states(inst, &read_buf);

	if (offset == 0) {
		LOG_HEXDUMP_DBG(read_buf.data, read_buf.len, "Call state");
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, read_buf.data, read_buf.len);
}

static void call_state_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
		inst->notify_call_states = (value == BT_GATT_CCC_NOTIFY);
	}
}

static int notify_ccp(struct bt_conn *conn, const struct bt_gatt_attr *attr, uint8_t call_index,
		      uint8_t opcode, uint8_t status)
{
	const struct bt_tbs_call_cp_notify ccp_not = {
		.call_index = call_index, .opcode = opcode, .status = status};

	LOG_DBG("Notifying CCP: Call index %u, %s opcode and status %s", call_index,
		bt_tbs_opcode_str(opcode), bt_tbs_status_str(status));

	return bt_gatt_notify(conn, attr, &ccp_not, sizeof(ccp_not));
}

static void hold_other_calls(struct tbs_inst *inst, uint8_t call_index_cnt,
			     const uint8_t *call_indexes)
{
	held_calls_cnt = 0;

	for (int i = 0; i < ARRAY_SIZE(inst->calls); i++) {
		bool hold_call = true;
		uint8_t call_state;

		for (int j = 0; j < call_index_cnt; j++) {
			if (inst->calls[i].index == call_indexes[j]) {
				hold_call = false;
				break;
			}
		}

		if (!hold_call) {
			continue;
		}

		call_state = inst->calls[i].state;
		if (call_state == BT_TBS_CALL_STATE_ACTIVE) {
			inst->calls[i].state = BT_TBS_CALL_STATE_LOCALLY_HELD;
			held_calls[held_calls_cnt++] = &inst->calls[i];
		} else if (call_state == BT_TBS_CALL_STATE_REMOTELY_HELD) {
			inst->calls[i].state = BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD;
			held_calls[held_calls_cnt++] = &inst->calls[i];
		}
	}
}

static uint8_t accept_call(struct tbs_inst *inst, const struct bt_tbs_call_cp_acc *ccp)
{
	struct bt_tbs_call *call = lookup_call_in_inst(inst, ccp->call_index);

	if (call == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	if (call->state == BT_TBS_CALL_STATE_INCOMING) {
		call->state = BT_TBS_CALL_STATE_ACTIVE;

		hold_other_calls(inst, 1, &ccp->call_index);

		return BT_TBS_RESULT_CODE_SUCCESS;
	} else {
		return BT_TBS_RESULT_CODE_STATE_MISMATCH;
	}
}

static uint8_t terminate_call(struct tbs_inst *inst, const struct bt_tbs_call_cp_term *ccp,
			      uint8_t reason)
{
	struct bt_tbs_call *call = lookup_call_in_inst(inst, ccp->call_index);

	if (call == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	call_free(call);
	tbs_set_terminate_reason(inst, ccp->call_index, reason);

	if (!inst_is_gtbs(inst)) {
		/* If the instance is different than the GTBS we set the termination reason and
		 * notify on the GTBS instance as well
		 */
		tbs_set_terminate_reason(&gtbs_inst, ccp->call_index, reason);
	}

	return BT_TBS_RESULT_CODE_SUCCESS;
}

static uint8_t tbs_hold_call(struct tbs_inst *inst, const struct bt_tbs_call_cp_hold *ccp)
{
	struct bt_tbs_call *call = lookup_call_in_inst(inst, ccp->call_index);

	if ((inst->optional_opcodes & BT_TBS_FEATURE_HOLD) == 0) {
		return BT_TBS_RESULT_CODE_OPCODE_NOT_SUPPORTED;
	}

	if (call == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	if (call->state == BT_TBS_CALL_STATE_ACTIVE) {
		call->state = BT_TBS_CALL_STATE_LOCALLY_HELD;
	} else if (call->state == BT_TBS_CALL_STATE_REMOTELY_HELD) {
		call->state = BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD;
	} else if (call->state == BT_TBS_CALL_STATE_INCOMING) {
		call->state = BT_TBS_CALL_STATE_LOCALLY_HELD;
	} else {
		return BT_TBS_RESULT_CODE_STATE_MISMATCH;
	}

	return BT_TBS_RESULT_CODE_SUCCESS;
}

static uint8_t retrieve_call(struct tbs_inst *inst, const struct bt_tbs_call_cp_retrieve *ccp)
{
	struct bt_tbs_call *call = lookup_call_in_inst(inst, ccp->call_index);

	if ((inst->optional_opcodes & BT_TBS_FEATURE_HOLD) == 0) {
		return BT_TBS_RESULT_CODE_OPCODE_NOT_SUPPORTED;
	}

	if (call == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	if (call->state == BT_TBS_CALL_STATE_LOCALLY_HELD) {
		call->state = BT_TBS_CALL_STATE_ACTIVE;
	} else if (call->state == BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD) {
		call->state = BT_TBS_CALL_STATE_REMOTELY_HELD;
	} else {
		return BT_TBS_RESULT_CODE_STATE_MISMATCH;
	}

	hold_other_calls(inst, 1, &ccp->call_index);

	return BT_TBS_RESULT_CODE_SUCCESS;
}

static int originate_call(struct tbs_inst *inst, const struct bt_tbs_call_cp_originate *ccp,
			  uint16_t uri_len, uint8_t *call_index)
{
	struct bt_tbs_call *call;

	/* Only allow one active outgoing call */
	for (int i = 0; i < CONFIG_BT_TBS_MAX_CALLS; i++) {
		if (inst->calls[i].state == BT_TBS_CALL_STATE_ALERTING) {
			return BT_TBS_RESULT_CODE_OPERATION_NOT_POSSIBLE;
		}
	}

	if (!bt_tbs_valid_uri(ccp->uri, uri_len)) {
		return BT_TBS_RESULT_CODE_INVALID_URI;
	}

	call = call_alloc(inst, BT_TBS_CALL_STATE_DIALING, ccp->uri, uri_len);
	if (call == NULL) {
		return BT_TBS_RESULT_CODE_OUT_OF_RESOURCES;
	}

	BT_TBS_CALL_FLAG_SET_OUTGOING(call->flags);

	hold_other_calls(inst, 1, &call->index);

	notify_calls(inst);
	call->state = BT_TBS_CALL_STATE_ALERTING;
	notify_calls(inst);

	LOG_DBG("New call with call index %u", call->index);

	*call_index = call->index;
	return BT_TBS_RESULT_CODE_SUCCESS;
}

static uint8_t join_calls(struct tbs_inst *inst, const struct bt_tbs_call_cp_join *ccp,
			  uint16_t call_index_cnt)
{
	struct bt_tbs_call *joined_calls[CONFIG_BT_TBS_MAX_CALLS];
	uint8_t call_state;

	if ((inst->optional_opcodes & BT_TBS_FEATURE_JOIN) == 0) {
		return BT_TBS_RESULT_CODE_OPCODE_NOT_SUPPORTED;
	}

	/* Check length */
	if (call_index_cnt < 2 || call_index_cnt > CONFIG_BT_TBS_MAX_CALLS) {
		return BT_TBS_RESULT_CODE_OPERATION_NOT_POSSIBLE;
	}

	/* Check for duplicates */
	for (int i = 0; i < call_index_cnt; i++) {
		for (int j = 0; j < i; j++) {
			if (ccp->call_indexes[i] == ccp->call_indexes[j]) {
				return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
			}
		}
	}

	/* Validate that all calls are in a joinable state */
	for (int i = 0; i < call_index_cnt; i++) {
		joined_calls[i] = lookup_call_in_inst(inst, ccp->call_indexes[i]);
		if (joined_calls[i] == NULL) {
			return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
		}

		call_state = joined_calls[i]->state;

		if (call_state == BT_TBS_CALL_STATE_INCOMING) {
			return BT_TBS_RESULT_CODE_OPERATION_NOT_POSSIBLE;
		}

		if (call_state != BT_TBS_CALL_STATE_LOCALLY_HELD &&
		    call_state != BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD &&
		    call_state != BT_TBS_CALL_STATE_ACTIVE) {
			return BT_TBS_RESULT_CODE_STATE_MISMATCH;
		}
	}

	/* Join all calls */
	for (int i = 0; i < call_index_cnt; i++) {
		call_state = joined_calls[i]->state;

		if (call_state == BT_TBS_CALL_STATE_LOCALLY_HELD) {
			joined_calls[i]->state = BT_TBS_CALL_STATE_ACTIVE;
		} else if (call_state == BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD) {
			joined_calls[i]->state = BT_TBS_CALL_STATE_REMOTELY_HELD;
		} else if (call_state == BT_TBS_CALL_STATE_INCOMING) {
			joined_calls[i]->state = BT_TBS_CALL_STATE_ACTIVE;
		}
		/* else active => Do nothing */
	}

	hold_other_calls(inst, call_index_cnt, ccp->call_indexes);

	return BT_TBS_RESULT_CODE_SUCCESS;
}

static void notify_app(struct bt_conn *conn, struct tbs_inst *inst, uint16_t len,
		       const union bt_tbs_call_cp_t *ccp, uint8_t status, uint8_t call_index)
{
	if (tbs_cbs == NULL) {
		return;
	}

	switch (ccp->opcode) {
	case BT_TBS_CALL_OPCODE_ACCEPT:
		if (tbs_cbs->accept_call != NULL) {
			tbs_cbs->accept_call(conn, call_index);
		}
		break;
	case BT_TBS_CALL_OPCODE_TERMINATE:
		if (tbs_cbs->terminate_call != NULL) {
			tbs_cbs->terminate_call(conn, call_index, inst->terminate_reason.reason);
		}
		break;
	case BT_TBS_CALL_OPCODE_HOLD:
		if (tbs_cbs->hold_call != NULL) {
			tbs_cbs->hold_call(conn, call_index);
		}
		break;
	case BT_TBS_CALL_OPCODE_RETRIEVE:
		if (tbs_cbs->retrieve_call != NULL) {
			tbs_cbs->retrieve_call(conn, call_index);
		}
		break;
	case BT_TBS_CALL_OPCODE_ORIGINATE: {
		char uri[CONFIG_BT_TBS_MAX_URI_LENGTH + 1];
		const uint16_t uri_len = len - sizeof(ccp->originate);
		bool remote_party_alerted = false;
		struct bt_tbs_call *call;

		call = lookup_call_in_inst(inst, call_index);

		if (call == NULL) {
			LOG_DBG("Could not find call by call index 0x%02x", call_index);
			break;
		}

		(void)memcpy(uri, ccp->originate.uri, uri_len);
		uri[uri_len] = '\0';
		if (tbs_cbs->originate_call != NULL) {
			remote_party_alerted = tbs_cbs->originate_call(conn, call_index, uri);
		}

		if (remote_party_alerted) {
			call->state = BT_TBS_CALL_STATE_ALERTING;
		} else {
			const struct bt_tbs_call_cp_term term = {
				.call_index = call_index, .opcode = BT_TBS_CALL_OPCODE_TERMINATE};

			/* Terminate and remove call */
			terminate_call(inst, &term, BT_TBS_REASON_CALL_FAILED);
		}

		notify_calls(inst);

		break;
	}
	case BT_TBS_CALL_OPCODE_JOIN: {
		const uint16_t call_index_cnt = len - sizeof(ccp->join);

		/* Let the app know about joined calls */
		if (tbs_cbs->join_calls != NULL) {
			tbs_cbs->join_calls(conn, call_index_cnt, ccp->join.call_indexes);
		}
		break;
	}
	default:
		break;
	}

	/* Let the app know about held calls */
	if (held_calls_cnt != 0 && tbs_cbs->hold_call != NULL) {
		for (int i = 0; i < held_calls_cnt; i++) {
			tbs_cbs->hold_call(conn, held_calls[i]->index);
		}
	}
}

static ssize_t write_call_cp(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);
	const union bt_tbs_call_cp_t *ccp = (union bt_tbs_call_cp_t *)buf;
	struct tbs_inst *tbs = NULL;
	uint8_t status;
	uint8_t call_index = 0;
	const bool is_gtbs = inst_is_gtbs(inst);

	if (!is_authorized(inst, conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len < sizeof(ccp->opcode)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	LOG_DBG("Index %u: Processing the %s opcode", inst_index(inst),
		bt_tbs_opcode_str(ccp->opcode));

	switch (ccp->opcode) {
	case BT_TBS_CALL_OPCODE_ACCEPT:
		if (len != sizeof(ccp->accept)) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		call_index = ccp->accept.call_index;

		if (is_gtbs) {
			tbs = lookup_inst_by_call_index(call_index);
			if (tbs == NULL) {
				status = BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
				break;
			}
		} else {
			tbs = inst;
		}

		status = accept_call(tbs, &ccp->accept);
		break;
	case BT_TBS_CALL_OPCODE_TERMINATE:
		if (len != sizeof(ccp->terminate)) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		call_index = ccp->terminate.call_index;

		if (is_gtbs) {
			tbs = lookup_inst_by_call_index(call_index);
			if (tbs == NULL) {
				status = BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
				break;
			}
		} else {
			tbs = inst;
		}

		status = terminate_call(tbs, &ccp->terminate, BT_TBS_REASON_CLIENT_TERMINATED);
		break;
	case BT_TBS_CALL_OPCODE_HOLD:
		if (len != sizeof(ccp->hold)) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		call_index = ccp->hold.call_index;

		if (is_gtbs) {
			tbs = lookup_inst_by_call_index(call_index);
			if (tbs == NULL) {
				status = BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
				break;
			}
		} else {
			tbs = inst;
		}

		status = tbs_hold_call(tbs, &ccp->hold);
		break;
	case BT_TBS_CALL_OPCODE_RETRIEVE:
		if (len != sizeof(ccp->retrieve)) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		call_index = ccp->retrieve.call_index;

		if (is_gtbs) {
			tbs = lookup_inst_by_call_index(call_index);
			if (tbs == NULL) {
				status = BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
				break;
			}
		} else {
			tbs = inst;
		}

		status = retrieve_call(tbs, &ccp->retrieve);
		break;
	case BT_TBS_CALL_OPCODE_ORIGINATE: {
		const uint16_t uri_len = len - sizeof(ccp->originate);

		if (len < sizeof(ccp->originate) + BT_TBS_MIN_URI_LEN) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		if (is_gtbs) {
			tbs = lookup_inst_by_uri_scheme(ccp->originate.uri, uri_len);
			if (tbs == NULL) {
				status = BT_TBS_RESULT_CODE_INVALID_URI;
				break;
			}
		} else {
			tbs = inst;
		}

		status = originate_call(tbs, &ccp->originate, uri_len, &call_index);
		break;
	}
	case BT_TBS_CALL_OPCODE_JOIN: {
		const uint16_t call_index_cnt = len - sizeof(ccp->join);

		if (len < sizeof(ccp->join) + 1) { /* at least 1 call index */
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		call_index = ccp->join.call_indexes[0];

		if (is_gtbs) {
			tbs = lookup_inst_by_call_index(call_index);
			if (tbs == NULL) {
				status = BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
				break;
			}
		} else {
			tbs = inst;
		}

		status = join_calls(tbs, &ccp->join, call_index_cnt);
		break;
	}
	default:
		status = BT_TBS_RESULT_CODE_OPCODE_NOT_SUPPORTED;
		call_index = 0;
		break;
	}

	if (tbs != NULL) {
		LOG_DBG("Index %u: Processed the %s opcode with status %s for call index %u",
			inst_index(inst), bt_tbs_opcode_str(ccp->opcode), bt_tbs_status_str(status),
			call_index);

		if (status == BT_TBS_RESULT_CODE_SUCCESS) {
			const struct bt_tbs_call *call = lookup_call(call_index);

			if (call != NULL) {
				LOG_DBG("Call is now in the %s state",
					bt_tbs_state_str(call->state));
			} else {
				LOG_DBG("Call is now terminated");
			}
		}
	}

	if (status != BT_TBS_RESULT_CODE_SUCCESS) {
		call_index = 0;
	}

	if (conn != NULL) {
		notify_ccp(conn, attr, call_index, ccp->opcode, status);
	} /* else local operation; don't notify */

	if (tbs != NULL && status == BT_TBS_RESULT_CODE_SUCCESS) {
		notify_calls(tbs);
		notify_app(conn, tbs, len, ccp, status, call_index);
	}

	return len;
}

static void call_cp_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_optional_opcodes(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				     void *buf, uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);
	const uint16_t optional_opcodes_le = sys_cpu_to_le16(inst->optional_opcodes);

	LOG_DBG("Index %u: Supported opcodes 0x%02x", inst_index(inst), inst->optional_opcodes);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &optional_opcodes_le,
				 sizeof(optional_opcodes_le));
}

static void terminate_reason_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_friendly_name(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				  uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);
	const struct bt_tbs_in_uri *friendly_name = &inst->friendly_name;
	size_t val_len;

	LOG_DBG("Index: 0x%02x call index 0x%02x, URI %s", inst_index(inst),
		friendly_name->call_index, friendly_name->uri);

	if (friendly_name->call_index == BT_TBS_FREE_CALL_INDEX) {
		LOG_DBG("URI not set");
		return bt_gatt_attr_read(conn, attr, buf, len, offset, NULL, 0);
	}

	val_len = sizeof(friendly_name->call_index) + strlen(friendly_name->uri);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, friendly_name, val_len);
}

static void friendly_name_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

static ssize_t read_incoming_call(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				  uint16_t len, uint16_t offset)
{
	const struct tbs_inst *inst = BT_AUDIO_CHRC_USER_DATA(attr);
	const struct bt_tbs_in_uri *remote_uri = &inst->in_call;
	size_t val_len;

	LOG_DBG("Index: 0x%02x call index 0x%02x, URI %s", inst_index(inst), remote_uri->call_index,
		remote_uri->uri);

	if (remote_uri->call_index == BT_TBS_FREE_CALL_INDEX) {
		LOG_DBG("URI not set");

		return bt_gatt_attr_read(conn, attr, buf, len, offset, NULL, 0);
	}

	val_len = sizeof(remote_uri->call_index) + strlen(remote_uri->uri);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, remote_uri, val_len);
}

static void in_call_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	const struct tbs_inst *inst = lookup_inst_by_attr(attr);

	if (inst != NULL) {
		LOG_DBG("Index %u: value 0x%04x", inst_index(inst), value);
	}
}

#define BT_TBS_CHR_PROVIDER_NAME(inst)                                                             \
	BT_AUDIO_CHRC(BT_UUID_TBS_PROVIDER_NAME, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,          \
		      BT_GATT_PERM_READ_ENCRYPT, read_provider_name, NULL, inst),                  \
		BT_AUDIO_CCC(provider_name_cfg_changed)

#define BT_TBS_CHR_UCI(inst)                                                                       \
	BT_AUDIO_CHRC(BT_UUID_TBS_UCI, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT, read_uci,     \
		      NULL, inst)

#define BT_TBS_CHR_TECHNOLOGY(inst)                                                                \
	BT_AUDIO_CHRC(BT_UUID_TBS_TECHNOLOGY, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,             \
		      BT_GATT_PERM_READ_ENCRYPT, read_technology, NULL, inst),                     \
		BT_AUDIO_CCC(technology_cfg_changed)

#define BT_TBS_CHR_URI_LIST(inst)                                                                  \
	BT_AUDIO_CHRC(BT_UUID_TBS_URI_LIST, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,               \
		      BT_GATT_PERM_READ_ENCRYPT, read_uri_scheme_list, NULL, inst),                \
		BT_AUDIO_CCC(uri_scheme_list_cfg_changed)

#define BT_TBS_CHR_SIGNAL_STRENGTH(inst)                                                           \
	BT_AUDIO_CHRC(BT_UUID_TBS_SIGNAL_STRENGTH, /* Optional */                                  \
		      BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,          \
		      read_signal_strength, NULL, inst),                                           \
		BT_AUDIO_CCC(signal_strength_cfg_changed)

#define BT_TBS_CHR_SIGNAL_INTERVAL(inst)                                                           \
	BT_AUDIO_CHRC(BT_UUID_TBS_SIGNAL_INTERVAL, /* Conditional */                               \
		      BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,    \
		      BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,                      \
		      read_signal_strength_interval, write_signal_strength_interval, inst)

#define BT_TBS_CHR_CURRENT_CALLS(inst)                                                             \
	BT_AUDIO_CHRC(BT_UUID_TBS_LIST_CURRENT_CALLS, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,     \
		      BT_GATT_PERM_READ_ENCRYPT, read_current_calls, NULL, inst),                  \
		BT_AUDIO_CCC(current_calls_cfg_changed)

#define BT_TBS_CHR_CCID(inst)                                                                      \
	BT_AUDIO_CHRC(BT_UUID_CCID, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT, read_ccid, NULL, \
		      inst)

#define BT_TBS_CHR_STATUS_FLAGS(inst)                                                              \
	BT_AUDIO_CHRC(BT_UUID_TBS_STATUS_FLAGS, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,           \
		      BT_GATT_PERM_READ_ENCRYPT, read_status_flags, NULL, inst),                   \
		BT_AUDIO_CCC(status_flags_cfg_changed)

#define BT_TBS_CHR_INCOMING_URI(inst)                                                              \
	BT_AUDIO_CHRC(BT_UUID_TBS_INCOMING_URI, /* Optional */                                     \
		      BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,          \
		      read_incoming_uri, NULL, inst),                                              \
		BT_AUDIO_CCC(incoming_uri_cfg_changed)

#define BT_TBS_CHR_CALL_STATE(inst)                                                                \
	BT_AUDIO_CHRC(BT_UUID_TBS_CALL_STATE, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,             \
		      BT_GATT_PERM_READ_ENCRYPT, read_call_state, NULL, inst),                     \
		BT_AUDIO_CCC(call_state_cfg_changed)

#define BT_TBS_CHR_CONTROL_POINT(inst)                                                             \
	BT_AUDIO_CHRC(BT_UUID_TBS_CALL_CONTROL_POINT,                                              \
		      BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_WRITE_WITHOUT_RESP,  \
		      BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_call_cp, inst),                      \
		BT_AUDIO_CCC(call_cp_cfg_changed)

#define BT_TBS_CHR_OPTIONAL_OPCODES(inst)                                                          \
	BT_AUDIO_CHRC(BT_UUID_TBS_OPTIONAL_OPCODES, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,  \
		      read_optional_opcodes, NULL, inst)

#define BT_TBS_CHR_TERMINATE_REASON(inst)                                                          \
	BT_AUDIO_CHRC(BT_UUID_TBS_TERMINATE_REASON, BT_GATT_CHRC_NOTIFY,                           \
		      BT_GATT_PERM_READ_ENCRYPT, NULL, NULL, inst),                                \
		BT_AUDIO_CCC(terminate_reason_cfg_changed)

#define BT_TBS_CHR_INCOMING_CALL(inst)                                                             \
	BT_AUDIO_CHRC(BT_UUID_TBS_INCOMING_CALL, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,          \
		      BT_GATT_PERM_READ_ENCRYPT, read_incoming_call, NULL, inst),                  \
		BT_AUDIO_CCC(in_call_cfg_changed)

#define BT_TBS_CHR_FRIENDLY_NAME(inst)                                                             \
	BT_AUDIO_CHRC(BT_UUID_TBS_FRIENDLY_NAME, /* Optional */                                    \
		      BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,          \
		      read_friendly_name, NULL, inst),                                             \
		BT_AUDIO_CCC(friendly_name_cfg_changed)

#define BT_TBS_SERVICE_DEFINE(_uuid, _inst)                                                        \
	BT_GATT_PRIMARY_SERVICE(_uuid), BT_TBS_CHR_PROVIDER_NAME(_inst), BT_TBS_CHR_UCI(_inst),    \
		BT_TBS_CHR_TECHNOLOGY(_inst), BT_TBS_CHR_URI_LIST(_inst),                          \
		BT_TBS_CHR_SIGNAL_STRENGTH(_inst), BT_TBS_CHR_SIGNAL_INTERVAL(_inst),              \
		BT_TBS_CHR_CURRENT_CALLS(_inst), BT_TBS_CHR_CCID(_inst),                           \
		BT_TBS_CHR_STATUS_FLAGS(_inst), BT_TBS_CHR_INCOMING_URI(_inst),                    \
		BT_TBS_CHR_CALL_STATE(_inst), BT_TBS_CHR_CONTROL_POINT(_inst),                     \
		BT_TBS_CHR_OPTIONAL_OPCODES(_inst), BT_TBS_CHR_TERMINATE_REASON(_inst),            \
		BT_TBS_CHR_INCOMING_CALL(_inst), BT_TBS_CHR_FRIENDLY_NAME(_inst)

#define BT_TBS_SERVICE_DEFINITION(_inst)                                                           \
	{                                                                                          \
		BT_TBS_SERVICE_DEFINE(BT_UUID_TBS, &(_inst))                                       \
	}

static struct bt_gatt_service gtbs_svc =
	BT_GATT_SERVICE(((struct bt_gatt_attr[]){BT_TBS_SERVICE_DEFINE(BT_UUID_GTBS, &gtbs_inst)}));

BT_GATT_SERVICE_INSTANCE_DEFINE(tbs_service_list, svc_insts, CONFIG_BT_TBS_BEARER_COUNT,
				BT_TBS_SERVICE_DEFINITION);

static void signal_interval_timeout(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct tbs_inst *inst = CONTAINER_OF(dwork, struct tbs_inst, reporting_interval_work);

	if (!inst->pending_signal_strength_notification) {
		return;
	}

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_SIGNAL_STRENGTH, inst->attrs, &inst->signal_strength,
			    sizeof(inst->signal_strength));

	if (inst->signal_strength_interval) {
		k_work_reschedule(&inst->reporting_interval_work,
				  K_SECONDS(inst->signal_strength_interval));
	}

	inst->pending_signal_strength_notification = false;
}

static int tbs_inst_init_and_register(struct tbs_inst *inst, struct bt_gatt_service *svc,
				      const struct bt_tbs_register_param *param)
{
	int ret;

	LOG_DBG("inst %p index 0x%02x", inst, inst_index(inst));

	ret = bt_ccid_alloc_value();
	if (ret < 0) {
		LOG_DBG("Could not allocate CCID: %d", ret);
		return ret;
	}

	inst->ccid = (uint8_t)ret;
	(void)utf8_lcpy(inst->provider_name, param->provider_name, sizeof(inst->provider_name));
	(void)utf8_lcpy(inst->uci, param->uci, sizeof(inst->uci));
	(void)utf8_lcpy(inst->uri_scheme_list, param->uri_schemes_supported,
			sizeof(inst->uri_scheme_list));
	inst->optional_opcodes = param->supported_features;
	inst->technology = param->technology;
	inst->attrs = svc->attrs;
	inst->attr_count = svc->attr_count;
	inst->authorization_required = param->authorization_required;

	k_work_init_delayable(&inst->reporting_interval_work, signal_interval_timeout);

	ret = bt_gatt_service_register(svc);
	if (ret != 0) {
		LOG_DBG("Could not register %sTBS: %d", param->gtbs ? "G" : "", ret);
		memset(inst, 0, sizeof(*inst));

		return ret;
	}

	return inst_index(inst);
}

static int gtbs_service_inst_register(const struct bt_tbs_register_param *param)
{
	return tbs_inst_init_and_register(&gtbs_inst, &gtbs_svc, param);
}

static int tbs_service_inst_register(const struct bt_tbs_register_param *param)
{
	for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
		struct tbs_inst *inst = &svc_insts[i];

		if (!(inst_is_registered(inst))) {
			return tbs_inst_init_and_register(inst, &tbs_service_list[i], param);
		}
	}

	return -ENOMEM;
}

static bool valid_register_param(const struct bt_tbs_register_param *param)
{
	size_t str_len;

	if (param == NULL) {
		LOG_DBG("param is NULL");

		return false;
	}

	if (param->provider_name == NULL) {
		LOG_DBG("provider_name is NULL");

		return false;
	}

	str_len = strlen(param->provider_name);
	if (str_len > CONFIG_BT_TBS_MAX_PROVIDER_NAME_LENGTH) {
		LOG_DBG("Provider name length (%zu) larger than "
			"CONFIG_BT_TBS_MAX_PROVIDER_NAME_LENGTH %d",
			str_len, CONFIG_BT_TBS_MAX_PROVIDER_NAME_LENGTH);

		return false;
	}

	if (param->uci == NULL) {
		LOG_DBG("uci is NULL");

		return false;
	}

	if (param->uri_schemes_supported == NULL) {
		LOG_DBG("uri_schemes_supported is NULL");

		return false;
	}

	if (!IN_RANGE(param->technology, BT_TBS_TECHNOLOGY_3G, BT_TBS_TECHNOLOGY_WCDMA)) {
		LOG_DBG("Invalid technology: %u", param->technology);

		return false;
	}

	if (param->supported_features > BT_TBS_FEATURE_ALL) {
		LOG_DBG("Invalid supported_features: %u", param->supported_features);

		return false;
	}

	if (CONFIG_BT_TBS_BEARER_COUNT == 0 && !param->gtbs) {
		LOG_DBG("Cannot register TBS when CONFIG_BT_TBS_BEARER_COUNT=0");

		return false;
	}

	return true;
}

int bt_tbs_register_bearer(const struct bt_tbs_register_param *param)
{
	int ret = -ENOEXEC;

	CHECKIF(!valid_register_param(param)) {
		LOG_DBG("Invalid parameters");

		return -EINVAL;
	}

	if (param->gtbs && inst_is_registered(&gtbs_inst)) {
		LOG_DBG("GTBS already registered");

		return -EALREADY;
	}

	if (!param->gtbs && !inst_is_registered(&gtbs_inst)) {
		LOG_DBG("GTBS not yet registered");

		return -EAGAIN;
	}

	if (param->gtbs) {
		ret = gtbs_service_inst_register(param);
		if (ret < 0) {
			LOG_DBG("Failed to register GTBS: %d", ret);

			return -ENOEXEC;
		}
	} else if (CONFIG_BT_TBS_BEARER_COUNT > 0) {
		ret = tbs_service_inst_register(param);
		if (ret < 0) {
			LOG_DBG("Failed to register GTBS: %d", ret);

			if (ret == -ENOMEM) {
				return -ENOMEM;
			}

			return -ENOEXEC;
		}
	}

	/* ret will contain the index of the registered service */
	return ret;
}

int bt_tbs_unregister_bearer(uint8_t bearer_index)
{
	struct tbs_inst *inst = inst_lookup_index(bearer_index);
	struct bt_gatt_service *svc;
	struct k_work_sync sync;
	bool restart_reporting_interval;
	int err;

	if (inst == NULL) {
		LOG_DBG("Could not find inst by index %u", bearer_index);

		return -EINVAL;
	}

	if (!inst_is_registered(inst)) {
		LOG_DBG("Instance from index %u is not registered", bearer_index);

		return -EALREADY;
	}

	if (inst_is_gtbs(inst)) {
		for (size_t i = 0; i < ARRAY_SIZE(svc_insts); i++) {
			struct tbs_inst *tbs = &svc_insts[i];

			if (inst_is_registered(tbs)) {
				LOG_DBG("TBS[%u] is registered, please unregister all TBS first",
					bearer_index);
				return -EAGAIN;
			}
		}
		svc = &gtbs_svc;
	} else {
		svc = &tbs_service_list[bearer_index];
	}

	restart_reporting_interval =
		k_work_cancel_delayable_sync(&inst->reporting_interval_work, &sync);

	err = bt_gatt_service_unregister(svc);
	if (err != 0) {
		LOG_DBG("Failed to unregister service %p: %d", svc, err);

		if (restart_reporting_interval && inst->signal_strength_interval != 0U) {
			/* In this unlikely scenario we may report interval later than expected if
			 * the k_work was cancelled right before it was set to trigger. It is not a
			 * big deal and not worth trying to reschedule in a way that it would
			 * trigger at the same time again, as specific timing over GATT is a wishful
			 * dream anyways
			 */
			k_work_schedule(&inst->reporting_interval_work,
					K_SECONDS(inst->signal_strength_interval));
		}

		return -ENOEXEC;
	}

	memset(inst, 0, sizeof(*inst));

	return 0;
}

int bt_tbs_accept(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	int status = -EINVAL;
	const struct bt_tbs_call_cp_acc ccp = {.call_index = call_index,
					       .opcode = BT_TBS_CALL_OPCODE_ACCEPT};

	if (inst != NULL) {
		status = accept_call(inst, &ccp);
	}

	if (status == BT_TBS_RESULT_CODE_SUCCESS) {
		notify_calls(inst);
	}

	return status;
}

int bt_tbs_hold(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	int status = -EINVAL;
	const struct bt_tbs_call_cp_hold ccp = {.call_index = call_index,
						.opcode = BT_TBS_CALL_OPCODE_HOLD};

	if (inst != NULL) {
		status = tbs_hold_call(inst, &ccp);
	}

	notify_calls(inst);

	return status;
}

int bt_tbs_retrieve(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	int status = -EINVAL;
	const struct bt_tbs_call_cp_retrieve ccp = {.call_index = call_index,
						    .opcode = BT_TBS_CALL_OPCODE_RETRIEVE};

	if (inst != NULL) {
		status = retrieve_call(inst, &ccp);
	}

	notify_calls(inst);

	return status;
}

int bt_tbs_terminate(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	int status = -EINVAL;
	const struct bt_tbs_call_cp_term ccp = {.call_index = call_index,
						.opcode = BT_TBS_CALL_OPCODE_TERMINATE};

	if (inst != NULL) {
		status = terminate_call(inst, &ccp, BT_TBS_REASON_SERVER_ENDED_CALL);
	}

	notify_calls(inst);

	return status;
}

int bt_tbs_originate(uint8_t bearer_index, char *remote_uri, uint8_t *call_index)
{
	struct tbs_inst *tbs = inst_lookup_index(bearer_index);
	uint8_t buf[CONFIG_BT_TBS_MAX_URI_LENGTH + sizeof(struct bt_tbs_call_cp_originate)];
	struct bt_tbs_call_cp_originate *ccp = (struct bt_tbs_call_cp_originate *)buf;
	size_t uri_len;

	if (tbs == NULL) {
		LOG_DBG("Could not find TBS instance from index %u", bearer_index);
		return -EINVAL;
	} else if (!bt_tbs_valid_uri((uint8_t *)remote_uri, strlen(remote_uri))) {
		LOG_DBG("Invalid URI %s", remote_uri);
		return -EINVAL;
	}

	uri_len = strlen(remote_uri);

	ccp->opcode = BT_TBS_CALL_OPCODE_ORIGINATE;
	(void)memcpy(ccp->uri, remote_uri, uri_len);

	return originate_call(tbs, ccp, uri_len, call_index);
}

int bt_tbs_join(uint8_t call_index_cnt, uint8_t *call_indexes)
{
	struct tbs_inst *inst;
	uint8_t buf[CONFIG_BT_TBS_MAX_CALLS + sizeof(struct bt_tbs_call_cp_join)];
	struct bt_tbs_call_cp_join *ccp = (struct bt_tbs_call_cp_join *)buf;
	int status = -EINVAL;

	if (call_index_cnt != 0 && call_indexes != 0) {
		inst = lookup_inst_by_call_index(call_indexes[0]);
	} else {
		return status;
	}

	if (inst != NULL) {
		ccp->opcode = BT_TBS_CALL_OPCODE_JOIN;
		(void)memcpy(ccp->call_indexes, call_indexes,
			     MIN(call_index_cnt, CONFIG_BT_TBS_MAX_CALLS));

		status = join_calls(inst, ccp, call_index_cnt);
	}

	return status;
}

int bt_tbs_remote_answer(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	struct bt_tbs_call *call;

	if (inst == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	call = lookup_call_in_inst(inst, call_index);

	if (call == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	if (call->state == BT_TBS_CALL_STATE_ALERTING) {
		call->state = BT_TBS_CALL_STATE_ACTIVE;
		notify_calls(inst);
		return BT_TBS_RESULT_CODE_SUCCESS;
	} else {
		return BT_TBS_RESULT_CODE_STATE_MISMATCH;
	}
}

int bt_tbs_remote_hold(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	struct bt_tbs_call *call;
	uint8_t status;

	if (inst == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	call = lookup_call_in_inst(inst, call_index);

	if (call == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	if (call->state == BT_TBS_CALL_STATE_ACTIVE) {
		call->state = BT_TBS_CALL_STATE_REMOTELY_HELD;
		status = BT_TBS_RESULT_CODE_SUCCESS;
	} else if (call->state == BT_TBS_CALL_STATE_LOCALLY_HELD) {
		call->state = BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD;
		status = BT_TBS_RESULT_CODE_SUCCESS;
	} else {
		status = BT_TBS_RESULT_CODE_STATE_MISMATCH;
	}

	if (status == BT_TBS_RESULT_CODE_SUCCESS) {
		notify_calls(inst);
	}

	return status;
}

int bt_tbs_remote_retrieve(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	struct bt_tbs_call *call;
	int status;

	if (inst == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	call = lookup_call_in_inst(inst, call_index);

	if (call == NULL) {
		return BT_TBS_RESULT_CODE_INVALID_CALL_INDEX;
	}

	if (call->state == BT_TBS_CALL_STATE_REMOTELY_HELD) {
		call->state = BT_TBS_CALL_STATE_ACTIVE;
		status = BT_TBS_RESULT_CODE_SUCCESS;
	} else if (call->state == BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD) {
		call->state = BT_TBS_CALL_STATE_LOCALLY_HELD;
		status = BT_TBS_RESULT_CODE_SUCCESS;
	} else {
		status = BT_TBS_RESULT_CODE_STATE_MISMATCH;
	}

	if (status == BT_TBS_RESULT_CODE_SUCCESS) {
		notify_calls(inst);
	}

	return status;
}

int bt_tbs_remote_terminate(uint8_t call_index)
{
	struct tbs_inst *inst = lookup_inst_by_call_index(call_index);
	int status = -EINVAL;
	const struct bt_tbs_call_cp_term ccp = {.call_index = call_index,
						.opcode = BT_TBS_CALL_OPCODE_TERMINATE};

	if (inst != NULL) {
		status = terminate_call(inst, &ccp, BT_TBS_REASON_REMOTE_ENDED_CALL);
	}

	notify_calls(inst);

	return status;
}

static void tbs_inst_remote_incoming(struct tbs_inst *inst, const char *to, const char *from,
				     const char *friendly_name, const struct bt_tbs_call *call)
{
	size_t local_uri_ind_len;
	size_t remote_uri_ind_len;
	size_t friend_name_ind_len;

	__ASSERT_NO_MSG(to != NULL);
	__ASSERT_NO_MSG(from != NULL);

	local_uri_ind_len = strlen(to) + 1;
	remote_uri_ind_len = strlen(from) + 1;

	inst->in_call.call_index = call->index;
	(void)utf8_lcpy(inst->in_call.uri, from, sizeof(inst->in_call.uri));

	inst->incoming_uri.call_index = call->index;
	(void)utf8_lcpy(inst->incoming_uri.uri, to, sizeof(inst->incoming_uri.uri));

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_INCOMING_URI, inst->attrs, &inst->incoming_uri,
			    local_uri_ind_len);

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_INCOMING_CALL, inst->attrs, &inst->in_call,
			    remote_uri_ind_len);

	if (friendly_name) {
		inst->friendly_name.call_index = call->index;
		utf8_lcpy(inst->friendly_name.uri, friendly_name, sizeof(inst->friendly_name.uri));
		friend_name_ind_len = strlen(from) + 1;

		bt_gatt_notify_uuid(NULL, BT_UUID_TBS_FRIENDLY_NAME, inst->attrs,
				    &inst->friendly_name, friend_name_ind_len);
	} else {
		inst->friendly_name.call_index = BT_TBS_FREE_CALL_INDEX;
		bt_gatt_notify_uuid(NULL, BT_UUID_TBS_FRIENDLY_NAME, inst->attrs, NULL, 0);
	}
}

int bt_tbs_remote_incoming(uint8_t bearer_index, const char *to, const char *from,
			   const char *friendly_name)
{
	struct tbs_inst *inst = inst_lookup_index(bearer_index);
	struct bt_tbs_call *call = NULL;

	if (inst == NULL) {
		LOG_DBG("Could not find TBS instance from index %u", bearer_index);
		return -EINVAL;
	} else if (!bt_tbs_valid_uri((uint8_t *)to, strlen(to))) {
		LOG_DBG("Invalid \"to\" URI: %s", to);
		return -EINVAL;
	} else if (!bt_tbs_valid_uri((uint8_t *)from, strlen(from))) {
		LOG_DBG("Invalid \"from\" URI: %s", from);
		return -EINVAL;
	}

	call = call_alloc(inst, BT_TBS_CALL_STATE_INCOMING, (uint8_t *)from, strlen(from));
	if (call == NULL) {
		return -ENOMEM;
	}

	BT_TBS_CALL_FLAG_SET_INCOMING(call->flags);

	/* Notify TBS*/
	tbs_inst_remote_incoming(inst, to, from, friendly_name, call);

	if (!inst_is_gtbs(inst)) {
		/* If the instance is different than the GTBS we set the remote incoming and
		 * notify on the GTBS instance as well
		 */
		tbs_inst_remote_incoming(&gtbs_inst, to, from, friendly_name, call);
	}

	notify_calls(inst);

	LOG_DBG("New call with call index %u", call->index);

	return call->index;
}

int bt_tbs_set_bearer_provider_name(uint8_t bearer_index, const char *name)
{
	struct tbs_inst *inst = inst_lookup_index(bearer_index);
	const size_t len = strlen(name);

	if (len >= CONFIG_BT_TBS_MAX_PROVIDER_NAME_LENGTH || len == 0) {
		return -EINVAL;
	} else if (inst == NULL) {
		return -EINVAL;
	}

	if (strcmp(inst->provider_name, name) == 0) {
		return 0;
	}

	(void)utf8_lcpy(inst->provider_name, name, sizeof(inst->provider_name));

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_PROVIDER_NAME, inst->attrs, inst->provider_name,
			    strlen(inst->provider_name));
	return 0;
}

int bt_tbs_set_bearer_technology(uint8_t bearer_index, uint8_t new_technology)
{
	struct tbs_inst *inst = inst_lookup_index(bearer_index);

	if (new_technology < BT_TBS_TECHNOLOGY_3G || new_technology > BT_TBS_TECHNOLOGY_WCDMA) {
		return -EINVAL;
	} else if (inst == NULL) {
		return -EINVAL;
	}

	if (inst->technology == new_technology) {
		return 0;
	}

	inst->technology = new_technology;

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_TECHNOLOGY, inst->attrs, &inst->technology,
			    sizeof(inst->technology));

	return 0;
}

int bt_tbs_set_signal_strength(uint8_t bearer_index, uint8_t new_signal_strength)
{
	struct tbs_inst *inst = inst_lookup_index(bearer_index);
	uint32_t timer_status;

	if (new_signal_strength > BT_TBS_SIGNAL_STRENGTH_MAX &&
	    new_signal_strength != BT_TBS_SIGNAL_STRENGTH_UNKNOWN) {
		return -EINVAL;
	} else if (inst == NULL) {
		return -EINVAL;
	}

	if (inst->signal_strength == new_signal_strength) {
		return 0;
	}

	inst->signal_strength = new_signal_strength;
	inst->pending_signal_strength_notification = true;

	timer_status = k_work_delayable_remaining_get(&inst->reporting_interval_work);
	if (timer_status == 0) {
		k_work_reschedule(&inst->reporting_interval_work, K_NO_WAIT);
	}

	LOG_DBG("Index %u: Reporting signal strength in %d ms", bearer_index, timer_status);

	return 0;
}

int bt_tbs_set_status_flags(uint8_t bearer_index, uint16_t status_flags)
{
	struct tbs_inst *inst = inst_lookup_index(bearer_index);

	if (!BT_TBS_VALID_STATUS_FLAGS(status_flags)) {
		return -EINVAL;
	} else if (inst == NULL) {
		return -EINVAL;
	}

	if (inst->status_flags == status_flags) {
		return 0;
	}

	inst->status_flags = status_flags;

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_STATUS_FLAGS, inst->attrs, &status_flags,
			    sizeof(status_flags));
	return 0;
}

int bt_tbs_set_uri_scheme_list(uint8_t bearer_index, const char **uri_list, uint8_t uri_count)
{
	char uri_scheme_list[CONFIG_BT_TBS_MAX_SCHEME_LIST_LENGTH];
	size_t len = 0;
	struct tbs_inst *inst;

	NET_BUF_SIMPLE_DEFINE(uri_scheme_buf, READ_BUF_SIZE);

	if (bearer_index >= ARRAY_SIZE(svc_insts)) {
		return -EINVAL;
	}

	inst = &svc_insts[bearer_index];
	(void)memset(uri_scheme_list, 0, sizeof(uri_scheme_list));

	for (int i = 0; i < uri_count; i++) {
		if (len) {
			len++;
			if (len > sizeof(uri_scheme_list) - 1) {
				return -ENOMEM;
			}

			strcat(uri_scheme_list, ",");
		}

		len += strlen(uri_list[i]);
		if (len > sizeof(uri_scheme_list) - 1) {
			return -ENOMEM;
		}

		/* Store list in temp list in case something goes wrong */
		strcat(uri_scheme_list, uri_list[i]);
	}

	if (strcmp(inst->uri_scheme_list, uri_scheme_list) == 0) {
		/* identical; don't update or notify */
		return 0;
	}

	/* Store final result */
	(void)utf8_lcpy(inst->uri_scheme_list, uri_scheme_list, sizeof(inst->uri_scheme_list));

	LOG_DBG("TBS instance %u uri prefix list is now %s", bearer_index, inst->uri_scheme_list);

	bt_gatt_notify_uuid(NULL, BT_UUID_TBS_URI_LIST, inst->attrs, &inst->uri_scheme_list,
			    strlen(inst->uri_scheme_list));

	if (!inst_is_gtbs(inst)) {
		/* If the instance is different than the GTBS notify on the GTBS instance as well */
		net_buf_simple_add_mem(&uri_scheme_buf, gtbs_inst.uri_scheme_list,
				       strlen(gtbs_inst.uri_scheme_list));

		/* TODO: Make uri schemes unique */
		for (size_t i = 0U; i < ARRAY_SIZE(svc_insts); i++) {
			const size_t uri_len = strlen(svc_insts[i].uri_scheme_list);

			if (uri_scheme_buf.len + uri_len >= uri_scheme_buf.size) {
				LOG_WRN("Cannot fit all TBS instances in GTBS "
					"URI scheme list");
				break;
			}

			net_buf_simple_add_mem(&uri_scheme_buf, svc_insts[i].uri_scheme_list,
					       uri_len);
		}

		LOG_DBG("GTBS: URI scheme %.*s", uri_scheme_buf.len, uri_scheme_buf.data);

		bt_gatt_notify_uuid(NULL, BT_UUID_TBS_URI_LIST, gtbs_inst.attrs,
				    uri_scheme_buf.data, uri_scheme_buf.len);
	}
	return 0;
}

void bt_tbs_register_cb(struct bt_tbs_cb *cbs)
{
	tbs_cbs = cbs;
}

#if defined(CONFIG_BT_TBS_LOG_LEVEL_DBG)
void bt_tbs_dbg_print_calls(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(svc_insts); i++) {
		LOG_DBG("Bearer #%u", i);
		for (int j = 0; j < ARRAY_SIZE(svc_insts[i].calls); j++) {
			struct bt_tbs_call *call = &svc_insts[i].calls[j];

			if (call->index == BT_TBS_FREE_CALL_INDEX) {
				continue;
			}

			LOG_DBG("  Call #%u", call->index);
			LOG_DBG("    State: %s", bt_tbs_state_str(call->state));
			LOG_DBG("    Flags: 0x%02X", call->flags);
			LOG_DBG("    URI  : %s", call->remote_uri);
		}
	}
}
#endif /* defined(CONFIG_BT_TBS_LOG_LEVEL_DBG) */
