/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/lbs.h>
#include <bluetooth/services/mds.h>

#include <dk_buttons_and_leds.h>

#include <memfault/metrics/metrics.h>
#include <memfault/metrics/battery.h>
#include <memfault/core/trace_event.h>

#include "fuel_gauge.h"
#include "memfault_persist.h"

#define SLEEP_TIME_MS 500

//#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME     "NEW_LBS"
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define RUN_STATUS_LED DK_LED1
#define CON_STATUS_LED DK_LED2
#define USER_LED       DK_LED3

#define USER_BUTTON DK_BTN1_MSK

/* BLE connection state values reported to Memfault. */
#define BLE_CONN_STATE_ADVERTISING 0
#define BLE_CONN_STATE_CONNECTED   1

#if DT_NODE_EXISTS(DT_NODELABEL(npm1300_ek_pmic))
#define NPM13XX_DEVICE(dev) DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_##dev))
#elif DT_NODE_EXISTS(DT_NODELABEL(npm1304_ek_pmic))
#define NPM13XX_DEVICE(dev) DEVICE_DT_GET(DT_NODELABEL(npm1304_ek_##dev))
#else
#error "neither npm1300 nor npm1304 found in devicetree"
#endif

static const struct device *pmic = NPM13XX_DEVICE(pmic);
static const struct device *charger = NPM13XX_DEVICE(charger);
static volatile bool vbus_connected;

static bool app_button_state;
static struct k_work adv_work;
static uint32_t led_state_bitmask;
static bool charging_enabled = true;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_LBS_VAL),
};

/* Wraps dk_set_led*() so the on-board LED state is mirrored into a Memfault metric. */
static void set_led(int led_idx, bool on)
{
	dk_set_led(led_idx, on);

	WRITE_BIT(led_state_bitmask, led_idx, on);
	MEMFAULT_METRIC_SET_UNSIGNED(led_state_bitmask, led_state_bitmask);
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	MEMFAULT_METRIC_SET_UNSIGNED(ble_conn_state, BLE_CONN_STATE_ADVERTISING);

	printk("Advertising successfully started as %s\n", DEVICE_NAME);
}

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed, err 0x%02x %s\n", err, bt_hci_err_to_str(err));
		return;
	}

	printk("Connected\n");

	set_led(CON_STATUS_LED, true);
	MEMFAULT_METRIC_SET_UNSIGNED(ble_conn_state, BLE_CONN_STATE_CONNECTED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected, reason 0x%02x %s\n", reason, bt_hci_err_to_str(reason));

	set_led(CON_STATUS_LED, false);
}

static void recycled_cb(void)
{
	printk("Connection object available from previous conn. Disconnect is complete!\n");
	advertising_start();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
	.recycled     = recycled_cb,
};

static void app_led_cb(bool led_state)
{
	set_led(USER_LED, led_state);
}

static bool app_button_cb(void)
{
	return app_button_state;
}

static struct bt_lbs_cb lbs_callbacks = {
	.led_cb    = app_led_cb,
	.button_cb = app_button_cb,
};

/* Enables or disables charging at runtime via the nPM1300 charger's CHGR EN_SET/EN_CLR. */
static void set_charging_enabled(bool enable)
{
	struct sensor_value val = {
		.val1 = enable ? 1 : 0,
		.val2 = 0,
	};
	int err = sensor_attr_set(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT,
				  SENSOR_ATTR_CONFIGURATION, &val);

	if (err) {
		printk("Failed to %s charging (err %d)\n", enable ? "enable" : "disable", err);
		return;
	}

	charging_enabled = enable;
	printk("Charging %s (Button 0)\n", enable ? "enabled" : "disabled");
}

static void button_changed(uint32_t button_state, uint32_t has_changed)
{
	if (has_changed & USER_BUTTON) {
		uint32_t user_button_state = button_state & USER_BUTTON;

		bt_lbs_send_button_state(user_button_state);
		app_button_state = user_button_state ? true : false;

		if (user_button_state) {
			/* Button 0 pressed: toggle charging on/off. */
			set_charging_enabled(!charging_enabled);
		}
	}
}

static int init_button(void)
{
	int err;

	err = dk_buttons_init(button_changed);
	if (err) {
		printk("Cannot init buttons (err: %d)\n", err);
	}

	return err;
}

/* Open access: no pairing is required, so any connected central may pull Memfault data. */
static bool mds_access_enable(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	return true;
}

static const struct bt_mds_cb mds_cb = {
	.access_enable = mds_access_enable,
};

static void event_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (pins & BIT(NPM13XX_EVENT_VBUS_DETECTED)) {
		printk("Vbus connected\n");
		vbus_connected = true;
		MEMFAULT_TRACE_EVENT_WITH_LOG(vbus_state_changed, "Vbus connected");
	}

	if (pins & BIT(NPM13XX_EVENT_VBUS_REMOVED)) {
		printk("Vbus removed\n");
		vbus_connected = false;
		MEMFAULT_TRACE_EVENT_WITH_LOG(vbus_state_changed, "Vbus removed");
	}
}

int main(void)
{
	uint32_t blink_status = 0;
	int err;

	printk("Starting mem_lbs (BLE LBS + Memfault-over-BLE + nPM13xx fuel gauge)\n");

	if (!device_is_ready(pmic)) {
		printk("Pmic device not ready.\n");
		return 0;
	}

	if (!device_is_ready(charger)) {
		printk("Charger device not ready.\n");
		return 0;
	}

	/* Settings subsystem is initialized earlier, from memfault_persist.c's own
	 * SYS_INIT, so that Memfault's event storage restore hook can use it too.
	 */

	if (fuel_gauge_init(charger) < 0) {
		printk("Could not initialise fuel gauge.\n");
		return 0;
	}

	static struct gpio_callback event_cb;

	gpio_init_callback(&event_cb, event_callback,
			   BIT(NPM13XX_EVENT_VBUS_DETECTED) | BIT(NPM13XX_EVENT_VBUS_REMOVED));

	err = mfd_npm13xx_add_callback(pmic, &event_cb);
	if (err) {
		printk("Failed to add pmic callback.\n");
		return 0;
	}

	/* Initialise vbus detection status. */
	struct sensor_value val;
	int ret = sensor_attr_get(charger, SENSOR_CHAN_CURRENT, SENSOR_ATTR_UPPER_THRESH, &val);

	if (ret < 0) {
		return 0;
	}

	vbus_connected = (val.val1 != 0) || (val.val2 != 0);

	printk("PMIC device ok\n");

	err = dk_leds_init();
	if (err) {
		printk("LEDs init failed (err %d)\n", err);
		return 0;
	}

	err = init_button();
	if (err) {
		printk("Button init failed (err %d)\n", err);
		return 0;
	}

	err = bt_lbs_init(&lbs_callbacks);
	if (err) {
		printk("Failed to init LBS (err:%d)\n", err);
		return 0;
	}

	err = bt_mds_cb_register(&mds_cb);
	if (err) {
		printk("Memfault Diagnostic service callback registration failed (err %d)\n", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

	memfault_persist_init();

	bool was_charging = false;

	while (1) {
		struct fuel_gauge_data fg_data;

		set_led(RUN_STATUS_LED, (++blink_status) % 2);

		if (fuel_gauge_update(charger, vbus_connected, &fg_data) == 0) {
			/* battery_soc_pct/soh_pct/voltage and the soc-drop/discharge-duration
			 * metrics are Memfault's official Core Battery Metrics, populated
			 * automatically by CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX.
			 */
			MEMFAULT_METRIC_SET_UNSIGNED(battery_charging, fg_data.charging ? 1 : 0);

			if (fg_data.charging && !was_charging) {
				/* Required by the battery metrics module so it excludes
				 * charge-related SoC increases from the discharge-drop stats.
				 */
				memfault_metrics_battery_stopped_discharging();
			}
			was_charging = fg_data.charging;
		}

		/* Using constant sleep time to keep the sample simple.
		 * In a real application, the interval can be long when the battery is discharging
		 * at a slow rate. Reduce the interval during periods of higher discharge current
		 * (relative to the battery size). Also use shorter intervals during charging to
		 * facilitate state-of-health estimation.
		 */
		k_msleep(SLEEP_TIME_MS);
	}
}
