.. _mem_lbs_memfault:

mem_lbs: BLE LBS + Memfault-over-BLE + nPM1300 fuel gauge
###########################################################

.. contents::
   :local:
   :depth: 2

This sample combines three things on a single `nRF54L15 DK <nRF54L15 DK product page_>`_:

* The Bluetooth LE :ref:`LED Button Service (LBS) <lbs_readme>`, advertised as ``mem_lbs``.
* The Bluetooth LE Memfault Diagnostic Service (:ref:`peripheral_mds <peripheral_mds>`), used to
  upload Memfault data over BLE instead of Wi-Fi or cellular.
* The `nPM1300 <nPM1300 product website_>`_ fuel gauge, using the :ref:`nrfxlib:nrf_fuel_gauge`.

Requirements
************

The sample supports the following development kit exclusively:

.. table-from-sample-yaml::

The sample also requires an `nPM1300 EK <nPM1300 EK product page_>`_ that you need to connect to
the development kit as described in `Wiring`_.

Overview
********

The sample advertises as ``mem_lbs`` with open access (no pairing/bonding required). Once
connected, a central can:

* Read the button state and write the LED state through the LED Button Service.
* Discover the Memfault Diagnostic Service and pull queued Memfault chunks. A BLE-to-cloud
  gateway (for example a phone app that implements the Memfault BLE gateway protocol) reads the
  service's Authorization characteristic to learn the upload URL and the
  ``Memfault-Project-Key`` header value, then forwards the chunks it reads to the Memfault
  project configured through :kconfig:option:`CONFIG_MEMFAULT_NCS_PROJECT_KEY`.

Because the DK is not always BLE-connected, Memfault data is buffered locally until a central
connects: heartbeats and trace events first queue in a RAM ring buffer
(:kconfig:option:`CONFIG_MEMFAULT_EVENT_STORAGE_SIZE`), and :file:`src/memfault_persist.c`
additionally snapshots that buffer to flash (the same NVS-backed :file:`storage_partition`
already used for the fuel gauge state) every 5 minutes, restoring it on the next boot. This means
data collected while disconnected survives a reboot or power loss too, up to two limits:

* **Backlog reach**: with the default 6144-byte buffer and ~130 bytes/heartbeat, roughly the last
  47 minutes of heartbeats survive a reboot. Increase :kconfig:option:`CONFIG_MEMFAULT_EVENT_STORAGE_SIZE`
  (and add more ``mflt/sN`` parts in :file:`memfault_persist.c` — each part must stay under the
  ~4000-byte single-NVS-entry ceiling) for a longer reach.
* **Freshness window**: data generated after the last snapshot and lost to an abrupt power loss
  before the next one (up to 5 minutes) is not recoverable — this is a periodic best-effort
  backup, not a transactional log, and that trade-off is intentional here.

No RRAM write-endurance figure is documented in the nRF Connect SDK or nrfx HAL sources, so the
5-minute snapshot interval is a conservative default rather than a vendor-verified safe rate for
very long continuous deployments.

In parallel, the sample continuously polls the nPM1300 fuel gauge and pushes metrics into
Memfault, collected every :kconfig:option:`CONFIG_MEMFAULT_METRICS_HEARTBEAT_INTERVAL_SECS`
(60 seconds by default). Battery metrics use Memfault's official `Core Battery Metrics
<https://docs.memfault.com/docs/platform/memfault-core-metrics?platform=MCU#battery>`_, wired to
the nPM1300 fuel gauge through :kconfig:option:`CONFIG_MEMFAULT_NRF_PLATFORM_BATTERY_NPM13XX` —
this is what powers the Memfault dashboard's built-in Battery widget. Defining these metrics by
hand instead (a custom heartbeat metric that merely happens to share the same key name) does
**not** work: the dashboard's Battery widget also needs ``battery_soc_pct_drop`` and
``battery_discharge_duration_ms``, which only the official module computes.

.. list-table:: Memfault metrics
   :widths: auto
   :header-rows: 1

   * - Metric key
     - Description
     - Source
   * - ``battery_soc_pct``
     - Battery state of charge, in percent.
     - Memfault Core Battery Metrics
   * - ``battery_soc_pct_drop`` / ``battery_discharge_duration_ms``
     - SoC drop and elapsed time per discharge session, used by the dashboard's Battery widget.
     - Memfault Core Battery Metrics
   * - ``battery_voltage`` / ``battery_soh_pct``
     - Battery voltage (mV) and state of health (percent).
     - Memfault Core Battery Metrics (NCS extension)
   * - ``battery_charging``
     - Whether the battery is currently being charged (0 or 1).
     - Custom (:file:`memfault_config/memfault_metrics_heartbeat_config.def`)
   * - ``ble_conn_state``
     - Bluetooth LE state: 0 while advertising, 1 while connected.
     - Custom
   * - ``led_state_bitmask``
     - DK LED state, bit0..bit3 map to LED1..LED4 (1 = on).
     - Custom

LED usage on the DK:

* **LED 1** blinks at 1 Hz while the application is running.
* **LED 2** is on while a BLE central is connected.
* **LED 3** mirrors the LED Button Service's LED characteristic (controlled by the central).

Button usage on the DK:

* **Button 0** toggles charging on and off (enables/disables the nPM1300 charger at runtime),
  in addition to its normal LED Button Service button-state notification. Charging is enabled
  by default at boot.

Charge parameters (nPM1300 charger, in :file:`npm1300.overlay`) are set to match the
`LP502540 (500 mAh)` battery model in :file:`Battery/`:

* Termination voltage: 4.20 V, matching the model's OCV curve at SoC = 100%.
* Constant current (fast-charge) current: 250 mA (0.5C for a 500 mAh cell).

.. _mem_lbs_memfault_wiring:

Wiring
******

To connect your DK to the nPM1300 EK, complete the following steps:

#. Make the following connections on the EK:

   a. Remove all existing connections, including jumpers and USB-C cables.
   #. On the **P1** pin header, connect **VBATIN** and **VBAT** pins with a jumper.
   #. On the **P17** pin header, connect all LEDs with jumpers.
   #. On the **P13** pin header, connect **RSET1** and **VSET1** pins with a jumper.
   #. On the **P14** pin header, connect **RSET2** and **VSET2** pins with a jumper.
   #. If your battery does not include an NTC thermistor, connect the **NTC** and **10kOhm** pins on the **P3** pin header with a jumper.
   #. Connect a suitable battery to either the **J2** or **J1** connector.
   #. Connect a USB power supply to the **J3** connector on the nPM1300 EK.

   With these connections, the USB power supply is powering the EK, the BUCK regulators are enabled, and the I/O reference voltage is supplied by the DK as described in the next step.

   .. note::
      Battery charging will not begin if the NTC signal is disconnected.

#. Connect the nRF54L15 DK to the EK as in the following table:

   .. list-table:: nPM1300 EK connections.
      :widths: auto
      :header-rows: 1

      * - EK pins
        - nRF54L15 DK pins
      * - SDA
        - P1.11
      * - SCL
        - P1.12
      * - GPIO3
        - P0.04
      * - VDDIO
        - VDDIO
      * - GND
        - GND

.. note::
   The PMIC **GPIO3** interrupt pin assignment uses the DK's **BUTTON 3** pin. Do not press
   **BUTTON 3** while the nPM1300 EK is wired, and note that this sample's BLE button handling
   only uses **BUTTON 1**.

Building and running
*********************

.. |sample path| replace:: :file:`sales_conf/pmic_memfualt`

.. include:: /includes/build_and_run.txt

Build for the nRF54L15 DK with the nPM1300 extra devicetree overlay applied:

.. code-block:: bash

   west build -b nrf54l15dk/nrf54l15/cpuapp sales_conf/pmic_memfualt -- -DEXTRA_DTC_OVERLAY_FILE=npm1300.overlay

Testing
*******

|test_sample|

#. |connect_kit|
#. |connect_terminal|

If the initialization was successful, the terminal displays status information similar to the
following:

.. code-block:: console

   Starting mem_lbs (BLE LBS + Memfault-over-BLE + nPM13xx fuel gauge)
   PMIC device ok
   Bluetooth initialized
   Advertising successfully started as mem_lbs
   V: 4.101, I: 0.000, T: 23.06, SoC: 93.09, TTE: nan, TTF: nan, SoH: 100.00, Cycles: 0, Chg: 0.00

#. Use a BLE scanner app (for example nRF Connect for Mobile) to find and connect to ``mem_lbs``.
   No pairing is required.
#. Use the LED Button Service to control **LED 3** and read the state of **BUTTON 1**.
#. Discover the Memfault Diagnostic Service and, using a Memfault-compatible BLE gateway,
   forward the collected chunks to the Memfault project identified by
   :kconfig:option:`CONFIG_MEMFAULT_NCS_PROJECT_KEY`.
#. Optionally, use the Memfault shell over the console to inspect metrics locally:

   .. code-block:: console

      $ mflt get_core
      $ mflt export

To store the fuel gauge state in non-volatile memory, use the following shell command:

.. code-block:: console

   $ fuel_gauge_state_store
   Storing state after next update

The fuel gauge state will be stored in non-volatile memory after the next measurement has been performed, and restored on the next boot.
Use the following shell commands to view the stored state and clear it:

.. code-block:: console

   $ settings list
   fuel_gauge_state
   $ settings delete fuel_gauge_state
   $ settings list

Dependencies
************

The sample uses the following `sdk-nrfxlib`_ library:

* :ref:`nrfxlib:nrf_fuel_gauge`

In addition, it uses the following |NCS| libraries:

* :ref:`lbs_readme`
* Memfault Diagnostic Service (``bluetooth/services/mds``)
* :ref:`mod_memfault`
* :ref:`dk_buttons_and_leds_readme`

It also uses the following Zephyr libraries:

* :ref:`zephyr:logging_api`
* :ref:`zephyr:shell_api`
* :ref:`zephyr:bluetooth_api`
