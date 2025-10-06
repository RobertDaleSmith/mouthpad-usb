# Raytac MDBT50Q-CX-40 with NCS 3.1.0

## Background
- The **`raytac_mdbt50q_cx_40_dongle`** board is **upstream in Zephyr**, but **not yet included in NCS 3.1.0’s Zephyr fork**.  
- NCS pins Zephyr via the `sdk-nrf/west.yml` manifest. In 3.1.0, `sdk-zephyr` is locked to a commit that predates Raytac board support.  
- The dongle ships with **Nordic Open Bootloader**, not MCUboot. That means you **cannot just flash `zephyr.hex`**; you must package the app into a DFU `.zip` with `nrfutil`.  

---

## Checking Which Zephyr You’re On
From your NCS 3.1.0 workspace:

```bash
west list sdk-zephyr -f "{name} {revision} {path}"
west manifest --freeze | grep zephyr
```

That shows the exact Zephyr commit pinned by NCS. You’ll notice it doesn’t contain the Raytac board folder.

---

## Options to Build for Raytac Dongle

### 1. Copy the Board Definition into Your App (Recommended)
- Grab the upstream `boards/raytac/mdbt50q_cx_40_dongle/` directory from Zephyr `main`.  
- Drop it into your app:  
  ```
  app_root/boards/raytac/mdbt50q_cx_40_dongle/
  ```
- Then build with:
  ```bash
  west build -b raytac_mdbt50q_cx_40_dongle/nrf52840 samples/basic/blinky
  ```

This works even if your workspace Zephyr fork doesn’t know the board.

---

### 2. Build with Upstream Zephyr (for sanity-check firmware)
- Clone upstream Zephyr and run:
  ```bash
  west build -b raytac_mdbt50q_cx_40_dongle/nrf52840 samples/basic/blinky
  ```
- Package for **Open Bootloader DFU**:
  ```bash
  nrfutil nrf5sdk-tools pkg generate \
    --hw-version 52 \
    --sd-req 0x00 \
    --application build/zephyr/zephyr.hex \
    --application-version 1 \
    blinky.zip

  nrfutil nrf5sdk-tools dfu usb-serial -pkg blinky.zip -p <serial_port>
  ```

⚠️ Do **not** flash raw HEX — Open Bootloader requires DFU packages.

---

### 3. Switch to MCUboot (One-Time Setup)
If you prefer MCUboot, you can flash MCUboot onto the dongle via Open Bootloader once, then use `west flash` or `mcumgr` for later updates. Mixing formats (e.g. signing for MCUboot but still using Open Bootloader) causes the “flashes but won’t boot” symptom.

---

## DCDC / Regulator Notes
- Old guides reference `CONFIG_SOC_DCDC_NRF52X`.  
- In Zephyr ≥ 3.0, regulators are configured via Devicetree.  
- If you need to disable the high-voltage regulator, add an overlay:

```dts
/* boards/raytac_mdbt50q_cx_40_dongle.overlay */
&regulator_hv {
    status = "disabled";
};
```

Check the board’s DTS for the actual regulator node names.

---

## Prebuilt Images (for hardware sanity checks)
- **BLE Sniffer HEX** (`sniffer_nrf52840dongle_nrf52840_4.1.1.hex`) works on the CX-40.  
- Flash it with nRF Connect Programmer to confirm USB + radio + bootloader are healthy.  
- OpenThread RCP HEX images for nRF52840 dongle also run on CX-40 (minor pin diffs for LEDs/buttons).

---

## TL;DR
1. NCS 3.1.0 doesn’t ship the Raytac board yet.  
2. Copy the board definition locally, or build with upstream Zephyr.  
3. Always package with `nrfutil` for Open Bootloader, unless you reflash MCUboot.  
4. Ignore old `CONFIG_SOC_DCDC_NRF52X` advice — use a DT overlay if needed.  
5. Use Nordic’s prebuilt sniffer HEX to quickly confirm your dongle works.  
