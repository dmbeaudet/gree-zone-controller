# Gree Zone Controller — Build & Commissioning Guide
## Hardware: Gree FXE48HP230V1R32AH + 3× WK-010WC1 + ESP32-S3

---

## 1. Parts List

| Part | Qty | Notes |
|------|-----|-------|
| ESP32-S3 DevKit (38-pin) | 1 | Any ESP32-S3 with ≥3 UARTs |
| MAX485 RS485 module | 3 | One per thermostat zone |
| 10kΩ resistor | 1 | Voltage divider for CN RX |
| 20kΩ resistor | 1 | Voltage divider for CN RX |
| 3-channel 5V relay board | 1 | 24VAC-rated contacts |
| 24VAC 40VA transformer | 1 | Powers stats + relay coils |
| 24VAC→5V buck converter | 1 | Powers ESP32 (e.g. RAC02-05SK) |
| DIN rail enclosure | 1 | Replaces Emerson EMM-3 box |
| 4-pin JST-XH connector | 1 | Mates with CN port on GRZ19-A5 |
| CP2102 USB-UART dongle | 1 | For commissioning / firmware flash |

**Libraries (Arduino Library Manager):**
- `PubSubClient` by Nick O'Leary
- `EspSoftwareSerial` by Dirk Kaar

---

## 2. CN Port Wiring (CRITICAL — read before connecting)

The GRZ19-A5 board has a 4-pin JST connector (CN3 or CN4 area near the
WiFi module slot). Before connecting the ESP32, identify pin functions:

### Step 1 — Find GND and 5V with a multimeter
Power the air handler. Probe each pin relative to the air handler chassis ground.
- One pin will read 0V = GND
- One pin will read ~5V = VCC (do not connect to ESP32)
- Two remaining pins are TX and RX (will idle around 3.3–5V)

### Step 2 — Identify TX vs RX
Connect a USB-UART adapter (CP2102) to the two unknown pins:
- Set USB-UART to 4800 baud, 8E1
- With both pins connected to RX of the adapter, one will show traffic (7E 7E bytes)
  — that pin is the board's TX output
- The other pin is the board's RX input (data from WiFi module)

### Step 3 — Level shifting
The board TX outputs 5V logic. The ESP32 RX is 3.3V max.

```
Board TX (5V) ──┬── 10kΩ ──→ ESP32 CN_RX_PIN (3.3V)
                └── 20kΩ ──→ GND

Board RX ──────────────────→ ESP32 CN_TX_PIN (3.3V drives 5V UART OK at 4800 baud)
```

---

## 3. Thermostat Wiring Changes

### At each thermostat (WK-010WC1) — do all 3:

**Step A — Switch to RS485 mode in Installer Setup:**
1. Hold MODE + FAN buttons for 5 seconds to enter installer menu
2. Navigate to "Communication Type"
3. Change from "24V" to "RS485"
4. Save and exit

**Step B — Rewire the 5-conductor cable:**

| Wire | Was | Now |
|------|-----|-----|
| 1 | R (24V hot) | R (keep — powers thermostat display) |
| 2 | C (common) | C (keep) |
| 3 | W or Y | H1 (RS485 data +) |
| 4 | G or spare | H2 (RS485 data −) |
| 5 | spare | spare (cap off) |

At the thermostat: connect wires to R, C, H1, H2 terminals.

---

## 4. MAX485 Wiring (×3, one per zone)

```
ESP32 GPIO (zone n TX) ──→ DI  pin on MAX485
ESP32 GPIO (zone n RX) ←── RO  pin on MAX485
ESP32 GPIO (zone n DE) ──→ DE  pin on MAX485 (tied to RE)
                           RE  pin (tied to DE)
3.3V ──────────────────→  VCC
GND ───────────────────→  GND
MAX485 A ──────────────→  H1 at thermostat end of cable
MAX485 B ──────────────→  H2 at thermostat end of cable
```

**DE pin logic:**
- LOW  = MAX485 in receive mode  (default — listening to thermostat)
- HIGH = MAX485 in transmit mode (briefly, when responding to thermostat)

---

## 5. Damper Relay Wiring

The Emerson EMM-3 used open/close/common (3-wire) motor control.
We simplify to single-relay spring-return control:

```
Relay NO contact ──→ M1 (damper OPEN terminal)
Relay COM ─────────→ 24VAC hot
Damper COM ────────→ 24VAC common
```

Motor springs closed when relay de-energizes. This is fail-safe:
power loss = all dampers close = furnace stops (safe state).

---

## 6. Power Distribution

```
120VAC ──→ 24VAC 40VA transformer
           │
           ├──→ RAC02-05SK buck ──→ 5V ──→ ESP32 VIN
           │
           ├──→ Relay board VCC
           │
           └──→ R terminal (distributed to all 3 thermostats)
               C terminal (common, also relay return)
```

---

## 7. Commissioning Sequence

### Phase 1 — Listen (verify CN port comms)
1. Flash firmware with `STARTUP_PHASE = PHASE_LISTEN`
2. Open Serial Monitor at 115200 baud
3. Power up air handler ONLY (no thermostats yet)
4. Watch for lines like: `[CN RX len=0x2F valid=Y] 7E 7E 2F ...`
5. If you see valid 7E 7E packets: ✅ CN port confirmed
6. If nothing: recheck TX/RX polarity on CN connector

### Phase 2 — Poll (verify bidirectional comms)
1. Type `N` in Serial Monitor OR wait 60 seconds
2. Watch for `[CN TX]` lines (outgoing poll packets)
3. Watch for `[AH]` lines: `pwr:OFF mode:0x10 setpt:20°C indoor:22°C`
4. If indoor temp shows a plausible number: ✅ full comms confirmed

### Phase 3 — Control (full operation)
1. Wire up one thermostat (Zone 1) to MAX485
2. Type `N` to advance OR wait 30 seconds
3. Verify damper relay opens when thermostat calls for heat
4. Verify `[CTRL]` lines show correct fan speed vs active zone count
5. Add Zone 2 and Zone 3 thermostats
6. Verify all three zones operate independently

---

## 8. MQTT Topics (Home Assistant)

All topics retain last value (retained=true):

```
gree_zone/zone1/calling        1 or 0
gree_zone/zone1/setpoint_c     integer °C
gree_zone/zone1/room_temp_c    integer °C
gree_zone/zone1/damper         "open" or "closed"
gree_zone/zone1/comm_ok        1 or 0
gree_zone/zone2/...            (same structure)
gree_zone/zone3/...            (same structure)
gree_zone/airhandler/powered   1 or 0
gree_zone/airhandler/indoor_temp_c   integer °C
gree_zone/airhandler/setpoint_c      integer °C
gree_zone/airhandler/mode_raw  hex string e.g. "0xC1"
gree_zone/airhandler/comm_ok   1 or 0
gree_zone/phase                1, 2, or 3
```

---

## 9. Arduino IDE board setting required for Zone 2

Zone 2 uses UART0 remapped to its RS485 pins. For this to work without
conflicting with the debug Serial port, you must enable USB CDC On Boot:

**Arduino IDE:** Tools → USB CDC On Boot → **Enabled**

**arduino-cli:**
```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
  firmware/GreeZoneController
```

With this setting, `Serial` (debug output) moves to the native USB port
(D+/D−), and UART0 hardware is free for Zone 2 RS485 at full 8E1 parity.

---

## 10. Zone 3 — SC16IS752 hardware installation

Zone 3 thermostat RS485 is handled by the SC16IS752 I2C dual-UART expander.
The firmware driver is bundled in `sc16is752.h` — no external library needed.

**On-module crystal: 14.7456 MHz**
Baud rate divisor = 14,745,600 / (4800 × 16) = **192** (exact integer, no error)

### Wiring

```
SC16IS752 VCC  → 3.3V
SC16IS752 GND  → GND
SC16IS752 SDA  → GPIO 38  (PRESSURE_SDA_PIN — shared I2C bus with SDP810)
SC16IS752 SCL  → GPIO 39  (PRESSURE_SCL_PIN — shared I2C bus with SDP810)
SC16IS752 A0   → GND  ──┐  I2C address = 0x48
SC16IS752 A1   → GND  ──┘
SC16IS752 TXD  → Z3 MAX485 DI  (pin 4 of the Zone 3 MAX485 module)
SC16IS752 RXD  ← Z3 MAX485 RO  (pin 1 of the Zone 3 MAX485 module)
```

**Note:** The ESP32 Z3_TX_PIN (GPIO 8) and Z3_RX_PIN (GPIO 9) are no longer
used — leave them disconnected or repurpose them. The ESP32 Z3_DE_PIN
(GPIO 10) still connects to the Zone 3 MAX485 DE/RE pins for direction
control — this wire does not change.

### Commissioning Zone 3

After wiring, power-cycle the controller. The serial monitor will show:

```
[SC16IS752] chA OK — 4800 baud 8E1 (crystal=14745600Hz div=192)
```

If you see `SC16IS752 Init FAIL` instead, check SDA/SCL connections and
confirm A0/A1 are tied to GND (address 0x48). If the module is absent,
Zone 3 is automatically disabled; the other two zones continue normally.

---

## 11. Known open items (v0.2)

1. **Zone 3 SC16IS752** — see §10 above.

2. **Room temp byte (PKT_OFF_ROOM_TEMP)** — byte 20 is an unverified guess.
   Confirm during Phase 2 using the byte-scan output, then set
   `ROOM_TEMP_BYTE_VERIFIED true` in config.h.

3. **Fahrenheit support** — setpoints arrive in °C from WK-010WC1.
   Add F/C conversion if thermostats are configured to display °F.

4. **OTA firmware updates** — add ArduinoOTA before final enclosure
   install to avoid needing physical access for future updates.

5. **MQTT PID runtime tuning** — add MQTT subscribe topics for KP/KI/KD
   so gains can be adjusted from Home Assistant without a serial connection.
