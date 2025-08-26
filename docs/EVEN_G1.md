# Even G1 Smart Glasses – Firmware Integration Crib Sheet

This crib sheet distills the **EvenDemoApp** protocol docs, the **MentraOS** open-source app, and community notes into a practical reference for building an **nRF (Zephyr/NCS) central** that pairs to the G1 and sends/receives data.

---

## 1. Device Discovery & Pairing

- The G1 exposes **two BLE peripherals**: **Left arm** and **Right arm**.
- Each arm advertises the **Nordic UART Service (NUS)**:
  - **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - **Write (TX→glasses)**: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
  - **Notify (RX←glasses)**: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
- Maintain **two central connections** (`conn_left`, `conn_right`).

---

## 2. Initialization Sequence (per arm)

1. Connect: `bt_conn_create_le()`
2. Discover NUS service + RX/TX chars (`bt_gatt_discover()`)
3. **Exchange MTU**: request ~247 (`bt_gatt_exchange_mtu()`)
4. Enable TX **notifications** (set CCCD)
5. Optional: enable Data Length Extension, tune connection interval (~15–45 ms)

⚠️ Do MTU exchange **before enabling CCCD**, otherwise notifications may fail.

---

## 3. Send/Receive Rules

### General Sequencing
- **Left → ACK → Right** for most commands.
- **Mic commands** (`0x0E`) go **only to Right**.
- Some flows (bitmaps) can be sent Left & Right independently.

### Opcodes

#### Text (`0x4E`)
- Fields: `seq, total_package_num, current_package_num, newscreen, new_char_pos0/1, current_page_num, max_page_num, data`
- Demo app uses:
  - Width ≈ 488 px
  - Font ≈ 21
  - 5 lines/screen (first 3 lines in one packet, last 2 in another)
- `newscreen` upper nibble = mode (`0x70` = “Text Show”), lower nibble = display new content (`0x01`)

#### Bitmap (`0x15 / 0x20 / 0x16`)
- Image: **1-bit, 576×136 px**
- Slice into **194-byte chunks**
- **First packet only**: `[0x15, seq, 0x00, 0x1C, 0x00, 0x00, <194 bytes>]`
- Subsequent packets: `[0x15, seq, <194 bytes>]`
- After last chunk:
  - End marker: `[0x20, 0x0D, 0x0E]`
  - Wait OK/ACK
  - CRC: `[0x16, <CRC32-XZ big-endian>]` over **(address + entire pixel data)**

#### Mic / Even-AI
- Long-press event: receive `0xF5 0x17`
- Respond to **Right**: `[0x0E, 0x01]` (enable mic)
- Audio arrives as `0xF1 …` LC3 chunks with sequence numbers
- Stop with `[0x0E, 0x00]` or when stop event received

---

## 4. Inbound Event Opcodes

- `0xF5`: touch & control events  
  - e.g., Single-tap, Double-tap, Long-press (`0x17`)
- `0xF1`: mic audio data chunks
- `0x0E`: mic status response  
  - `0xC9` = success, `0xCA` = fail

---

## 5. Known Gotchas

- **Activation**: fresh G1s must be set up once with the **Even app** (for provisioning/firmware update). After that, third-party apps/firmware (EvenDemoApp, MentraOS, your nRF) can pair/talk directly.
- **MTU/CCCD order**: negotiate MTU before enabling notifications.
- **CRC**: must be **CRC32-XZ, big-endian**, computed over `00 1C 00 00 + image_data`.
- **Pixel data**: do **not** include BMP header/palette; send raw pixel array only.
- **Inter-packet timing**: if drops occur, insert ~3–10 ms delay between packets; wait for end ACK before sending CRC.
- **Dual radios**: Left and Right are independent; test Left first to validate your pipeline.

---

## 6. Development Flow

1. Activate/update G1 once with the Even app.
2. Scan + connect to both arms with your nRF central.
3. Exchange MTU (247), enable NUS notifications.
4. Implement opcode parser + sender state machine.
5. Validate text (`0x4E`) first.
6. Add bitmap flow (`0x15/0x20/0x16`).
7. Add mic events (`0x0E`, `0xF1`, `0xF5`).

---

## 7. Example Bitmap Transfer (hex dump)

Suppose we’re sending the first 194-byte chunk of an all-zero image.

- **First packet (seq=0):**

```
15 00 00 1C 00 00 00 00 00 00 00 00 00 00 ... [total 194 data bytes]
```

- **Subsequent packet (seq=1):**

```
15 01 00 00 00 00 ... [next 194 data bytes]
```

- **End marker:**

```
20 0D 0E
```

- **CRC (example for all-zero image 576x136):**

```
16 58 8D 52 3A
```

(where `58 8D 52 3A` is the CRC32-XZ in **big-endian**)

---

## 8. References

- [EvenDemoApp (official sample)](https://github.com/even-realities/EvenDemoApp) – protocol documentation and examples
- [MentraOS (open source glasses OS)](https://github.com/Mentra-Community/MentraOS) – shows 3rd-party pairing/communication works out of the box
- [g1_uart_mcp (community NUS client)](https://github.com/danroblewis/g1_uart_mcp) – confirms NUS UUIDs and packet plumbing

