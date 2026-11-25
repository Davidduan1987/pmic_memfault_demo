/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef __MEMFAULT_PERSIST_H__
#define __MEMFAULT_PERSIST_H__

/* Starts the periodic flash snapshot of the Memfault event storage buffer. */
void memfault_persist_init(void);

#endif /* __MEMFAULT_PERSIST_H__ */
