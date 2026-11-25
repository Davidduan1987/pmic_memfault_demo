/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Persists the Memfault event-storage RAM ring buffer (heartbeat metrics and
 * trace events) into the existing NVS-backed settings partition, so data
 * collected while BLE is disconnected survives a reboot or power loss and is
 * still uploaded via BT_MDS the next time a BLE central connects.
 *
 * The snapshot is a periodic best-effort backup, not a transactional log: an
 * abrupt power loss between two snapshots only loses the data generated
 * since the last one, which is an accepted trade-off here.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#include <memfault/core/event_storage.h>
#include <memfault/core/platform/overrides.h>

#include "memfault_persist.h"

/* CONFIG_MEMFAULT_EVENT_STORAGE_SIZE bytes, split into NVS-entry-sized halves. */
#define MFLT_STORAGE_PART_COUNT 2
#define MFLT_STORAGE_PART_SIZE  (CONFIG_MEMFAULT_EVENT_STORAGE_SIZE / MFLT_STORAGE_PART_COUNT)

#define MFLT_SNAPSHOT_INTERVAL K_MINUTES(5)

#define SETTINGS_KEY_CTX "mflt/ctx"
#define SETTINGS_KEY_STORAGE_FMT "mflt/s%d"

BUILD_ASSERT(CONFIG_MEMFAULT_EVENT_STORAGE_SIZE % MFLT_STORAGE_PART_COUNT == 0,
	     "CONFIG_MEMFAULT_EVENT_STORAGE_SIZE must split evenly into parts");

static void snapshot_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(snapshot_work, snapshot_work_handler);

static int settings_early_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		printk("Settings subsystem could not be initialized (err %d).\n", err);
	}

	return err;
}

/* Must run before Memfault's own SYS_INIT (APPLICATION level), which calls
 * memfault_event_storage_restore_state() below and therefore needs settings
 * already mounted. POST_KERNEL always runs before APPLICATION regardless of
 * priority number; 60 just needs to be after the flash driver's own
 * POST_KERNEL init (CONFIG_FLASH_INIT_PRIORITY, default 50).
 */
SYS_INIT(settings_early_init, POST_KERNEL, 60);

bool memfault_event_storage_restore_state(sMfltEventStorageSaveState *state)
{
	ssize_t len = settings_load_one(SETTINGS_KEY_CTX, state->context, state->context_len);

	if (len != (ssize_t)state->context_len) {
		printk("No saved Memfault event storage state found, starting fresh\n");
		return false;
	}

	for (int i = 0; i < MFLT_STORAGE_PART_COUNT; i++) {
		char key[16];

		snprintf(key, sizeof(key), SETTINGS_KEY_STORAGE_FMT, i);

		len = settings_load_one(key, (uint8_t *)state->storage + i * MFLT_STORAGE_PART_SIZE,
					MFLT_STORAGE_PART_SIZE);
		if (len != MFLT_STORAGE_PART_SIZE) {
			printk("Saved Memfault event storage state incomplete, starting fresh\n");
			return false;
		}
	}

	printk("Restored Memfault event storage state from flash\n");
	return true;
}

static void snapshot_work_handler(struct k_work *work)
{
	memfault_lock();
	sMfltEventStorageSaveState state = memfault_event_storage_get_state();

	int err = settings_save_one(SETTINGS_KEY_CTX, state.context, state.context_len);

	for (int i = 0; err == 0 && i < MFLT_STORAGE_PART_COUNT; i++) {
		char key[16];

		snprintf(key, sizeof(key), SETTINGS_KEY_STORAGE_FMT, i);

		err = settings_save_one(key, (uint8_t *)state.storage + i * MFLT_STORAGE_PART_SIZE,
					MFLT_STORAGE_PART_SIZE);
	}
	memfault_unlock();

	if (err) {
		printk("Failed to snapshot Memfault event storage to flash (err %d)\n", err);
	} else {
		printk("Memfault event storage snapshotted to flash\n");
	}

	k_work_reschedule(&snapshot_work, MFLT_SNAPSHOT_INTERVAL);
}

void memfault_persist_init(void)
{
	k_work_reschedule(&snapshot_work, MFLT_SNAPSHOT_INTERVAL);
}
