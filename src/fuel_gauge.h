/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef __FUEL_GAUGE_H__
#define __FUEL_GAUGE_H__

struct fuel_gauge_data {
	float soc;
	float soh;
	bool charging;
};

int fuel_gauge_init(const struct device *charger);

int fuel_gauge_update(const struct device *charger, bool vbus_connected,
		       struct fuel_gauge_data *data);

#endif /* __FUEL_GAUGE_H__ */
