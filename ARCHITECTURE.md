# ESP32 IoT v3 — System Architecture

## Overview

`esp32-iot-v3` is an ESP-IDF firmware that bridges an **Advance elevator controller** (over RS485) with cloud services (over MQTT/Wi‑Fi). The ESP32 acts as a protocol gateway: it parses PCG frames on both sides, routes them by peer type and port, and locally handles register access (RTR) and firmware-image storage (FWU).

```
┌─────────────┐     MQTT      ┌──────────────────┐     RS485      ┌─────────────┐
│ Cloud peers │◄─────────────►│  ESP32 gateway   │◄──────────────►│   Advance   │
│ client /    │  Wi‑Fi TCP    │  (this firmware) │  UART2 HDX     │  controller │
│ servers     │               └──────────────────┘                └─────────────┘
└─────────────┘
```

---

## High-Level Components

| Layer | Responsibility |
| --- | --- |
| **Connectivity** | NVS, netif, Wi‑Fi (`protocol_examples_common`), MQTT client |
| **Gateway core** | `pcgtask` — main loop that merges MQTT and UART traffic |
| **Protocol** | `pcg` — frame build/parse, CRC-16, stream framing |
| **Serial / RTR** | `pcg_rtr` — RS485 UART init, register read, TX helper |
| **FWU storage** | `fwu` — read/write 256-byte blocks in the `fwu` flash partition |
| **MQTT buffer** | `mqtt_data` — FreeRTOS queue between MQTT ISR/event and `pcgtask` |
| **GPIO** | Status LEDs; RS485 DE/RE via UART RTS |

Active sources (see `main/CMakeLists.txt`):

```
main/
  app_main.c
  mqtt/mqtt_data.*
  gateway/pcgtask.*
  pcg/pcg.* , pcg_rtr.*
  fwu/fwu.*
  gpio/gpio_output.*
  legacy/          # unused older stack (not compiled)
```

---

## Boot Sequence

`app_main` starts the system in this order:

1. Initialize NVS, network interface, and the default event loop  
2. Configure status LEDs (`gpio_led_init`)  
3. Initialize UART2 in **RS485 half-duplex** mode (`pcg_uart_init`)  
4. Read device serial from Advance via RTR register `10100` (`rtrReadRegister`)  
5. Connect Wi‑Fi (`example_connect`)  
6. Create the MQTT data queue (`mqtt_data_queue_init`)  
7. Start the MQTT client and subscribe to `{serial}/pub`  
8. Start `pcgtask` on CPU core 1 (`pcgtask_start`)

On MQTT connect, the device publishes `"connected."` to `{serial}/connect`.

---

## Runtime Architecture

```
                    MQTT event handler
                           │
                           ▼
                   mqtt_data queue
                           │
                           ▼
┌──────────────────────────────────────────────────────────┐
│                      pcgtask (core 1)                      │
│                                                          │
│  • Dequeue MQTT payloads → parse PCG → route             │
│  • Poll UART RX → coalesce → split frames → route        │
│  • On DIP SYNCK → release one pending frame to RS485     │
│  • Local ports: DIP, FWU                                  │
└──────────────────────────────────────────────────────────┘
         │                                    │
         ▼                                    ▼
   esp_mqtt_client                     UART2 / RS485
   publish to topics                   rtr_rs485_send
```

`pcgtask` is the single consumer of both inbound paths. MQTT callbacks only enqueue; they never talk to UART or flash directly. That keeps the MQTT event loop short and serializes all PCG side effects on one task.

---

## PCG Protocol

Frames are binary packets with a fixed header, variable payload, and trailing CRC-16 (Modbus-style polynomial `0xA001`, init `0xFFFF`).

### Frame layout

| Offset | Field | Size |
| --- | --- | --- |
| 0 | Packet ID (`pid`) | 1 |
| 1 | Sender type | 1 |
| 2–3 | Sender serial (LE) | 2 |
| 4 | Receiver type | 1 |
| 5 | Receiver port | 1 |
| 6… | Payload | N |
| end−2 | CRC-16 (LE) | 2 |

Minimum size: **8 bytes**. Maximum considered when splitting a stream: **1024 bytes**.

### Peers (`pcg_peer_t`)

| Value | Name | Role |
| --- | --- | --- |
| 1 | `ADVANCE` | Elevator controller on RS485 |
| 2 | `ESP32` | This gateway |
| 3 | `CLIENT` | Remote client over MQTT |
| 4 | `SERVER_UPDATE` | Update server over MQTT |
| 5 | `SERVER_RECORD` | Record server over MQTT |

### Ports (`pcg_port_t`)

| Value | Name | Use |
| --- | --- | --- |
| 0 | `DIP` | Device/sync control (SYNCK) |
| 1 | `RTR` | Register transfer (read/write) |
| 2 | `FWU` | Firmware image block I/O |
| 3 | `MODBUS` | Reserved / passthrough capable |

Helpers:

- `pcg_parse` / `pcg_build` — single-frame CRC check and construction  
- `pcg_split_frames` — extract shortest CRC-valid frames from a byte stream  

---

## Routing Rules

### From MQTT (`{serial}/pub`)

1. Parse as PCG. Sender must be `CLIENT`, `SERVER_UPDATE`, or `SERVER_RECORD`.  
2. If receiver is **Advance** → push the raw frame into the **pending queue** (not sent immediately).  
3. If receiver is **ESP32** → handle locally (DIP / FWU).  
4. Otherwise ignore.

### From RS485 (Advance)

1. Coalesce UART bytes for `CONFIG_PCG_RX_FLUSH_MS` idle time, then split frames.  
2. Sender must be `ADVANCE`.  
3. If receiver is **ESP32** → handle locally (DIP / FWU).  
4. If receiver is a cloud peer → publish the original frame to:
   - `{serial}/client`
   - `{serial}/server_update`
   - `{serial}/server_record`

### SYNCK (DIP) flow

Advance sends a DIP packet with command `1` (SYNCK request) when it is ready for the next downlink frame.

1. `pcgtask` pops one frame from the pending queue (FIFO or LIFO per Kconfig).  
2. Optional delay (`CONFIG_PCG_SYNCK_DELAY_MS`).  
3. Frame is written on RS485 via `rtr_rs485_send`.  
4. While a SYNCK is in progress, further SYNCK requests are ignored (`s_synck_busy`).

This pacing keeps the half-duplex bus under Advance control and avoids flooding the controller.

---

## Local Services

### RTR — register transfer

Used at boot (and available as a library API) to read Advance registers over RS485:

- Builds a PCG/RTR read request (`mode=1`, one register address)  
- Retransmits every 1 s until a matching write-mode reply arrives or timeout  
- Device serial register: **`10100`**

RS485 TX uses UART2 with hardware DE/RE on the configured RTS GPIO.

### FWU — firmware update storage

Maps PCG port `FWU` onto a data partition labeled `fwu` (`partitions.csv`).

| Mode | Request | Reply |
| --- | --- | --- |
| READ (1) | mode + 16-byte address | WRITE (mode + address + 256 data) |
| WRITE (2) | mode + address + 256 data | ACK (mode + address) |

- Addresses must be **256-byte aligned**  
- Writes update flash with a 4 KB sector read-modify-erase-write cycle  
- Fully erased (0xFF) blocks are returned as zeros so bootloaders see size 0  

Replies go back to the original sender: RS485 if Advance, otherwise MQTT topic for that peer.

---

## MQTT Topic Map

Device identity is the Advance serial number read at boot (`deviceSerial`).

| Topic | Direction | Content |
| --- | --- | --- |
| `{serial}/connect` | ESP32 → broker | Connection announcement |
| `{serial}/pub` | broker → ESP32 | Inbound PCG frames |
| `{serial}/client` | ESP32 → broker | Frames for client |
| `{serial}/server_update` | ESP32 → broker | Frames for update server |
| `{serial}/server_record` | ESP32 → broker | Frames for record server |

Broker URI: `CONFIG_BROKER_URL_RAVIS` (menuconfig).

---

## Hardware / Flash Layout

### Default pins (menuconfig)

| Signal | Default GPIO |
| --- | --- |
| UART2 TX | 17 |
| UART2 RX | 16 |
| RS485 DE/RE (UART RTS) | 4 |
| LED 1 / LED 2 | 15 / 2 |

UART: **38400 8N1**, half-duplex RS485.

### Partition table (`partitions.csv`, ~2 MB flash)

| Name | Type | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | data | 24 KB | Non-volatile settings |
| `phy_init` | data | 4 KB | RF calibration |
| `factory` | app | 1 MB | Application firmware |
| `fwu` | data | ~960 KB | Elevator FW image staging |

---

## Configuration Knobs

Under **Example Configuration** in `idf.py menuconfig`:

| Option | Effect |
| --- | --- |
| Wi‑Fi SSID / password | Station credentials |
| Broker URL | MQTT broker URI |
| UART TX/RX / DE‑RE pins | RS485 wiring |
| Pending queue FIFO vs LIFO | SYNCK release order |
| Queue depth (1–16) | Max buffered MQTT→Advance frames |
| SYNCK delay (ms) | Pause before UART write after SYNCK |
| RX flush delay (ms) | UART idle coalesce window |

---

## Design Notes

- **Single router task** avoids races between MQTT and UART on the same bus and pending queue.  
- **SYNCK gating** treats Advance as the bus master for downlink timing.  
- **Opaque forwarding**: cloud↔Advance frames are published/queued as raw PCG bytes when not addressed to the ESP32.  
- **CRC-based framing** recovers from stream noise without a length field in the header.
