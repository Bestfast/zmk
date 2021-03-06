/*
 * Copyright (c) 2020 Peter Johanson
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/types.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>
#include <sys/byteorder.h>

#include <logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/split/bluetooth/uuid.h>
#include <zmk/event-manager.h>
#include <zmk/events/position-state-changed.h>
#include <init.h>

static int start_scan(void);

#define POSITION_STATE_DATA_LEN 16

static struct bt_conn *default_conn;

static struct bt_uuid_128 uuid = BT_UUID_INIT_128(ZMK_SPLIT_BT_SERVICE_UUID);
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;

static u8_t split_central_notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, u16_t length)
{
	static u8_t position_state[POSITION_STATE_DATA_LEN];

	u8_t changed_positions[POSITION_STATE_DATA_LEN];

	if (!data) {
		LOG_DBG("[UNSUBSCRIBED]");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	LOG_DBG("[NOTIFICATION] data %p length %u", data, length);

	for (int i = 0; i < POSITION_STATE_DATA_LEN; i++) {
		changed_positions[i] = ((u8_t *)data)[i] ^ position_state[i];
		position_state[i] = ((u8_t *)data)[i];
	}

	for (int i = 0; i < POSITION_STATE_DATA_LEN; i++) {
		for (int j = 0; j < 8; j++) {
			if (changed_positions[i] & BIT(j)) {
				u32_t position = (i * 8) + j;
				bool pressed = position_state[i] & BIT(j);
				struct position_state_changed *pos_ev = new_position_state_changed();
				pos_ev->position = position;
				pos_ev->state = pressed;

				LOG_DBG("Trigger key position state change for %d", position);
				ZMK_EVENT_RAISE(pos_ev);
			}
		}
	}


	return BT_GATT_ITER_CONTINUE;
}

static u8_t split_central_discovery_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		LOG_DBG("Discover complete");
		(void)memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);

	if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID))) {
		memcpy(&uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID), sizeof(uuid));
		discover_params.uuid = &uuid.uuid;
		discover_params.start_handle = attr->handle + 1;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			LOG_ERR("Discover failed (err %d)", err);
		}
	} else if (!bt_uuid_cmp(discover_params.uuid,
				BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID))) {
		memcpy(&uuid, BT_UUID_GATT_CCC, sizeof(uuid));
		discover_params.uuid = &uuid.uuid;
		discover_params.start_handle = attr->handle + 2;
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
		subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			LOG_ERR("Discover failed (err %d)", err);
		}
	} else {
		subscribe_params.notify = split_central_notify_func;
		subscribe_params.value = BT_GATT_CCC_NOTIFY;
		subscribe_params.ccc_handle = attr->handle;

		err = bt_gatt_subscribe(conn, &subscribe_params);
		if (err && err != -EALREADY) {
			LOG_ERR("Subscribe failed (err %d)", err);
		} else {
			LOG_DBG("[SUBSCRIBED]");
		}

		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_STOP;
}

static void split_central_process_connection(struct bt_conn *conn) {
	int err;

	LOG_DBG("Current security for connection: %d", bt_conn_get_security(conn));
	
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_ERR("Failed to set security (reason %d)", err);
		return;
	}

	if (conn == default_conn) {
		discover_params.uuid = &uuid.uuid;
		discover_params.func = split_central_discovery_func;
		discover_params.start_handle = 0x0001;
		discover_params.end_handle = 0xffff;
		discover_params.type = BT_GATT_DISCOVER_PRIMARY;

		err = bt_gatt_discover(default_conn, &discover_params);
		if (err) {
			LOG_ERR("Discover failed(err %d)", err);
			return;
		}
	}

	struct bt_conn_info info;

	bt_conn_get_info(conn, &info);

	LOG_DBG("New connection params: Interval: %d, Latency: %d, PHY: %d", info.le.interval, info.le.latency, info.le.phy->rx_phy);
}

static bool split_central_eir_found(struct bt_data *data, void *user_data)
{
	bt_addr_le_t *addr = user_data;
	int i;

	LOG_DBG("[AD]: %u data_len %u", data->type, data->data_len);

	switch (data->type) {
	case BT_DATA_UUID128_SOME:
	case BT_DATA_UUID128_ALL:
		if (data->data_len % 16 != 0U) {
			LOG_ERR("AD malformed");
			return true;
		}

		for (i = 0; i < data->data_len; i += 16) {
			struct bt_le_conn_param *param;
			struct bt_uuid uuid;
			int err;

            if (!bt_uuid_create(&uuid, &data->data[i], 16)) {
                LOG_ERR("Unable to load UUID");
                continue;
            }

			if (!bt_uuid_cmp(&uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID))) {
				char uuid_str[BT_UUID_STR_LEN];
				char service_uuid_str[BT_UUID_STR_LEN];

				bt_uuid_to_str(&uuid, uuid_str, sizeof(uuid_str));
				bt_uuid_to_str(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID), service_uuid_str, sizeof(service_uuid_str));
				LOG_DBG("UUID %s does not match split UUID: %s", log_strdup(uuid_str), log_strdup(service_uuid_str));
				continue;
			}

			LOG_DBG("Found the split service");

			err = bt_le_scan_stop();
			if (err) {
				LOG_ERR("Stop LE scan failed (err %d)", err);
				continue;
			}

			default_conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
			if (default_conn) {
				LOG_DBG("Found existing connection");
				split_central_process_connection(default_conn);
			} else {
				param = BT_LE_CONN_PARAM(0x0006, 0x0006, 30, 400);
				err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
							param, &default_conn);
				if (err) {
					LOG_ERR("Create conn failed (err %d)", err);
					start_scan();
				}

				err = bt_conn_le_phy_update(default_conn, BT_CONN_LE_PHY_PARAM_2M);
				if (err) {
					LOG_ERR("Update phy conn failed (err %d)", err);
					start_scan();
				}
			}

			return false;
		}
	}

	return true;
}

static void split_central_device_found(const bt_addr_le_t *addr, s8_t rssi, u8_t type,
			 struct net_buf_simple *ad)
{
	char dev[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, dev, sizeof(dev));
	LOG_DBG("[DEVICE]: %s, AD evt type %u, AD data len %u, RSSI %i",
	       log_strdup(dev), type, ad->len, rssi);

	/* We're only interested in connectable events */
	if (type == BT_GAP_ADV_TYPE_ADV_IND ||
	    type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		bt_data_parse(ad, split_central_eir_found, (void *)addr);
	}
}

static int start_scan(void)
{
	int err;

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, split_central_device_found);
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	LOG_DBG("Scanning successfully started");
    return 0;
}

static void split_central_connected(struct bt_conn *conn, u8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		LOG_ERR("Failed to connect to %s (%u)", addr, conn_err);

		bt_conn_unref(default_conn);
		default_conn = NULL;

		start_scan();
		return;
	}

	LOG_DBG("Connected: %s", log_strdup(addr));	

	split_central_process_connection(conn);
}

static void split_central_disconnected(struct bt_conn *conn, u8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_DBG("Disconnected: %s (reason %d)", log_strdup(addr), reason);

	if (default_conn != conn) {
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;

	start_scan();
}

static struct bt_conn_cb conn_callbacks = {
	.connected = split_central_connected,
	.disconnected = split_central_disconnected,
};

int zmk_split_bt_central_init(struct device *_arg)
{
	bt_conn_cb_register(&conn_callbacks);

	return start_scan();
}

SYS_INIT(zmk_split_bt_central_init,
         APPLICATION,
		 CONFIG_ZMK_BLE_INIT_PRIORITY);