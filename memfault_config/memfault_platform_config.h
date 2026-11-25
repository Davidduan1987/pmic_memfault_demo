/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 *
 * Platform overrides for the default configuration settings in the
 * memfault-firmware-sdk. Default configuration settings can be found in
 * "<NCS folder>/modules/lib/memfault-firmware-sdk/components/include/memfault/default_config.h"
 */

/* Enables memfault_event_storage_restore_state(), implemented in
 * src/memfault_persist.c, to reload the event storage ring buffer that was
 * periodically snapshotted to flash before the last reboot/power loss.
 */
#define MEMFAULT_EVENT_STORAGE_RESTORE_STATE 1
