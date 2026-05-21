# Gree Zone Controller — Complete Wiring Schematic
### ESP32-S3 DevKitC-1 · 3× MAX485 · SDP810-500Pa · 3-ch Relay Board

---

```
╔══════════════════════════════════════════════════════════════════════════════════════╗
║          GREE ZONE CONTROLLER — COMPLETE WIRING SCHEMATIC  (v0.2)                  ║
║   Hardware: ESP32-S3 DevKitC-1 + Gree FXE48HP230V1R32AH + 3× WK-010WC1            ║
╚══════════════════════════════════════════════════════════════════════════════════════╝


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  POWER  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  120VAC ──►  ┌──────────────────┐
              │  24VAC 40VA      │  24VAC HOT ──────────────────────────────────────────────────────┐
              │  Transformer     │                                                                   │
              └──────────────────┘  24VAC COM ──────────────────────────────────────────────────┐   │
                      │                                                                          │   │
                      │ 24VAC                                                                    │   │
                      ▼                                                                          │   │
              ┌──────────────────┐                                                               │   │
              │   RAC02-05SK     │                                                               │   │
              │   24VAC → 5V     │── 5V ──► ESP32-S3 VIN                                        │   │
              │   buck converter │── GND ──► GND Bus ◄──────────────────────────────────────────┘   │
              └──────────────────┘                                                                   │
                                                                                                     │
  ┌──────────────────────────────────────────────────────────────────────────────────────────────────┘
  │  24VAC HOT
  ├──► Thermostat Z1 — R terminal (24VAC power for display)
  ├──► Thermostat Z2 — R terminal
  ├──► Thermostat Z3 — R terminal
  └──► Relay board — COM contacts (24VAC hot through each NO contact → damper OPEN terminal)
  C (common) bus → all thermostat C terminals + damper COM terminals + relay return


━━━━━━━━━━━━━━━━━━━━━━━  CN PORT — AIR HANDLER  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  UART1 · 4800 baud · 8E1 · 5V logic

  Gree FXE48HP230V1R32AH                          10kΩ
  GRZ19-A5 board                     ┌────────────┤├────────────── GPIO 18  CN_RX  (3.3V in)
  CN port (4-pin JST-XH)             │            ┤├─┐
  ┌─────────────────┐                │  Board TX  resistor │
  │ pin 1  GND      │────────────────┼──────────────────── GND Bus
  │ pin 2  5V VCC   │  ← do NOT     │            20kΩ ─────────── GND
  │ pin 3  TX (5V)  │────────────────┘
  │ pin 4  RX (5V)  │─────────────────────────────────────── GPIO 17  CN_TX  (3.3V drives 5V ok)
  └─────────────────┘
  ↑ Identify pins with multimeter before connecting (see COMMISSIONING.md §2)

  Voltage divider detail:
    Board TX ──┬── 10kΩ ──► GPIO 18  (reads ~3.3V when TX is 5V HIGH)
               └── 20kΩ ──► GND      (divider: 5V × 20/(10+20) = 3.33V)


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ZONE 1  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  UART2 · HardwareSerial · 4800 baud · 8E1 · full parity support

  ESP32-S3                     MAX485-Z1 module              WK-010WC1 Thermostat Z1
  ┌───────────┐                ┌─────────────────┐           ┌──────────────────────┐
  │ GPIO 15   │────────────────│ DI  (data in)   │           │ R  ◄── 24VAC hot     │
  │ (Z1_TX)   │                │                 │A ─────────│ H1 (RS485+)          │
  │ GPIO 16   │────────────────│ RO  (data out)  │B ─────────│ H2 (RS485−)          │
  │ (Z1_RX)   │                │                 │           │ C  ◄── 24VAC common  │
  │ GPIO 14   │────────────────│ DE ─┐           │           └──────────────────────┘
  │ (Z1_DE)   │                │ RE ─┘ tied      │
  │           │                │                 │
  │ GPIO  4   │──────────────────────────────────────────────► Relay 1 IN
  │ (Z1_RELAY)│                │ VCC ◄── 3.3V    │             (active HIGH = Zone 1 damper OPEN)
  └───────────┘                │ GND ◄── GND Bus │
                               └─────────────────┘

  Relay 1 (24VAC rated):
    NO contact ──► Damper Z1 OPEN terminal
    COM        ──► 24VAC hot
    Damper COM ──► 24VAC common    (spring-return: power off = damper closed = fail-safe)


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ZONE 2  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  UART0 remapped · HardwareSerial · 4800 baud · 8E1
  ⚠  REQUIRES "USB CDC On Boot = Enabled" in platformio.ini (already set)
     Debug Serial moves to native USB-C port; UART0 hardware freed for Zone 2

  ESP32-S3                     MAX485-Z2 module              WK-010WC1 Thermostat Z2
  ┌───────────┐                ┌─────────────────┐           ┌──────────────────────┐
  │ GPIO 11   │────────────────│ DI  (data in)   │           │ R  ◄── 24VAC hot     │
  │ (Z2_TX)   │                │                 │A ─────────│ H1 (RS485+)          │
  │ GPIO 12   │────────────────│ RO  (data out)  │B ─────────│ H2 (RS485−)          │
  │ (Z2_RX)   │                │                 │           │ C  ◄── 24VAC common  │
  │ GPIO 13   │────────────────│ DE ─┐           │           └──────────────────────┘
  │ (Z2_DE)   │                │ RE ─┘ tied      │
  │           │                │                 │
  │ GPIO  5   │──────────────────────────────────────────────► Relay 2 IN
  │ (Z2_RELAY)│                │ VCC ◄── 3.3V    │             (active HIGH = Zone 2 damper OPEN)
  └───────────┘                │ GND ◄── GND Bus │
                               └─────────────────┘

  Relay 2 (24VAC rated):
    NO contact ──► Damper Z2 OPEN terminal
    COM        ──► 24VAC hot
    Damper COM ──► 24VAC common


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ZONE 3  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  SoftwareSerial (TEMPORARY) · 4800 baud · 8N1 only
  ⚠  8E1 parity NOT supported — Zone 3 thermostat comms unreliable until SC16IS752
     installed. See COMMISSIONING.md §10. SC16IS752 shares the I2C bus (GPIO 38/39).

  ESP32-S3                     MAX485-Z3 module              WK-010WC1 Thermostat Z3
  ┌───────────┐                ┌─────────────────┐           ┌──────────────────────┐
  │ GPIO  8   │────────────────│ DI  (data in)   │           │ R  ◄── 24VAC hot     │
  │ (Z3_TX)   │                │                 │A ─────────│ H1 (RS485+)          │
  │ GPIO  9   │────────────────│ RO  (data out)  │B ─────────│ H2 (RS485−)          │
  │ (Z3_RX)   │                │                 │           │ C  ◄── 24VAC common  │
  │ GPIO 10   │────────────────│ DE ─┐           │           └──────────────────────┘
  │ (Z3_DE)   │                │ RE ─┘ tied      │
  │           │                │                 │
  │ GPIO  6   │──────────────────────────────────────────────► Relay 3 IN
  │ (Z3_RELAY)│                │ VCC ◄── 3.3V    │             (active HIGH = Zone 3 damper OPEN)
  └───────────┘                │ GND ◄── GND Bus │
                               └─────────────────┘

  Relay 3 (24VAC rated):
    NO contact ──► Damper Z3 OPEN terminal
    COM        ──► 24VAC hot
    Damper COM ──► 24VAC common

  ── Future SC16IS752 upgrade (feat/zone3-sc16is752 branch) ──────────────────────────
  When SC16IS752 installed, GPIO 8/9 are freed. SC16IS752 wiring:
    SC16IS752 SDA  ──► GPIO 38  (shared I2C bus with SDP810)
    SC16IS752 SCL  ──► GPIO 39
    SC16IS752 A0   ──► GND      }  I2C address = 0x48
    SC16IS752 A1   ──► GND      }
    SC16IS752 VCC  ──► 3.3V
    SC16IS752 GND  ──► GND Bus
    SC16IS752 TXD  ──► MAX485-Z3 DI   (replaces GPIO 8)
    SC16IS752 RXD  ◄── MAX485-Z3 RO   (replaces GPIO 9)
    Crystal: 14.7456 MHz  →  divisor = 192  →  exact 4800 baud
  ────────────────────────────────────────────────────────────────────────────────────


━━━━━━━━━━━━━━━━━━━━━━  PRESSURE SENSOR (SDP810-500Pa)  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  I2C · address 0x25 · 4.7kΩ pull-ups on SDA and SCL

  ESP32-S3                     SDP810-500Pa
  ┌───────────┐                ┌────────────────────────────────────────────┐
  │ GPIO 38   │────────────────│ SDA  (I2C data)                            │
  │ (SDA)     │    4.7kΩ      │                                            │
  │           │──┤├──► 3.3V   │  High-pressure port ──► supply plenum tap  │
  │ GPIO 39   │────────────────│ SCL  (I2C clock)                           │
  │ (SCL)     │    4.7kΩ      │                                            │
  │           │──┤├──► 3.3V   │  Low-pressure port  ──► open to room air   │
  │ 3.3V      │────────────────│ VDD                                        │
  │ GND       │────────────────│ GND                                        │
  └───────────┘                └────────────────────────────────────────────┘
  NOTE: SDP810 and SC16IS752 share the same I2C bus (GPIO 38/39). Addresses
        do not conflict: SDP810 = 0x25, SC16IS752 = 0x48.


━━━━━━━━━━━━━━━━━━━━━━━━━━━━  STATUS LED  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  GPIO  2  ──► 330Ω ──► LED anode
                         LED cathode ──► GND
  Blink pattern:  1 s = Phase 1 Listen
                500 ms = Phase 2 Poll
                200 ms = Phase 3 running
                steady = Phase 3 idle


━━━━━━━━━━━━━━━━━━━━━━━  ESP32-S3 PIN SUMMARY  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  ┌──────┬────────────┬───────────────────────────────────────────┐
  │ GPIO │ Signal     │ Connected to                              │
  ├──────┼────────────┼───────────────────────────────────────────┤
  │   2  │ STATUS_LED │ 330Ω → LED → GND                         │
  │   4  │ Z1_RELAY   │ Relay 1 IN  (Zone 1 damper)              │
  │   5  │ Z2_RELAY   │ Relay 2 IN  (Zone 2 damper)              │
  │   6  │ Z3_RELAY   │ Relay 3 IN  (Zone 3 damper)              │
  │   8  │ Z3_TX      │ MAX485-Z3 DI  (SoftwareSerial TX)        │
  │   9  │ Z3_RX      │ MAX485-Z3 RO  (SoftwareSerial RX)        │
  │  10  │ Z3_DE      │ MAX485-Z3 DE+RE  (direction control)     │
  │  11  │ Z2_TX      │ MAX485-Z2 DI  (UART0 TX)                 │
  │  12  │ Z2_RX      │ MAX485-Z2 RO  (UART0 RX)                 │
  │  13  │ Z2_DE      │ MAX485-Z2 DE+RE  (direction control)     │
  │  14  │ Z1_DE      │ MAX485-Z1 DE+RE  (direction control)     │
  │  15  │ Z1_TX      │ MAX485-Z1 DI  (UART2 TX)                 │
  │  16  │ Z1_RX      │ MAX485-Z1 RO  (UART2 RX)                 │
  │  17  │ CN_TX      │ GRZ19-A5 RX pin  (3.3V → 5V ok)         │
  │  18  │ CN_RX      │ Voltage divider → GRZ19-A5 TX pin        │
  │  38  │ I2C SDA    │ SDP810 SDA + SC16IS752 SDA  (4.7kΩ↑3.3V)│
  │  39  │ I2C SCL    │ SDP810 SCL + SC16IS752 SCL  (4.7kΩ↑3.3V)│
  │  VIN │ 5V power   │ RAC02-05SK 5V output                     │
  │  GND │ Ground     │ GND bus (transformer C, all modules)     │
  └──────┴────────────┴───────────────────────────────────────────┘

  GPIO 43/44 (UART0 default TX/RX) — NOT connected. UART0 remapped to 11/12.
  Debug Serial output → native USB-C port (enabled by USB CDC On Boot flag).


━━━━━━━━━━━━━━━━━━━━━━━━  MAX485 MODULE WIRING (all 3 identical)  ━━━━━━━━━━━━━━━━━━

  MAX485 pin    Connects to
  ──────────    ───────────────────────────────────────────
  VCC           3.3V
  GND           GND Bus
  DI            ESP32 TX pin for that zone
  RO            ESP32 RX pin for that zone
  DE            ESP32 DE pin for that zone  ─┐ tie DE and RE
  RE            ESP32 DE pin for that zone  ─┘ together on module
  A             Thermostat H1 (RS485+)
  B             Thermostat H2 (RS485−)

  DE pin behaviour:
    LOW  (default) = MAX485 receives from thermostat
    HIGH (briefly) = MAX485 transmits 0x2F response to thermostat


━━━━━━━━━━━━━━━━━━━━━━━━━━━  RELAY BOARD  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Relay board input:
    VCC  ◄── 5V (from RAC02-05SK)
    GND  ◄── GND Bus
    IN1  ◄── GPIO 4   (Zone 1 — active HIGH triggers relay)
    IN2  ◄── GPIO 5   (Zone 2)
    IN3  ◄── GPIO 6   (Zone 3)

  Each relay output (spring-return damper motor):
    COM  ──► 24VAC hot  (from transformer)
    NO   ──► Damper OPEN terminal
             Damper COM ──► 24VAC common (C bus)

  Relay energised (GPIO HIGH) → NO closes → 24VAC to damper → damper OPENS
  Relay released  (GPIO LOW ) → NO opens  → no power → spring closes damper
  Power failure → all relays release → all dampers close → AH stops  ← FAIL-SAFE


━━━━━━━━━━━━━━━━━━━━━━  WK-010WC1 THERMOSTAT WIRING  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  (same for all 3 thermostats)

  ⚠ Thermostat must be switched to RS485 mode first:
    Hold MODE + FAN 5 s → Installer menu → Communication Type → RS485

  Terminal   Wire         Connects to
  ────────   ──────       ──────────────────────────────────────────────
  R          was R        24VAC hot  (powers thermostat display)
  C          was C        24VAC common
  H1         was W or Y   MAX485 A terminal  (RS485+)
  H2         was G        MAX485 B terminal  (RS485−)
  (5th wire) spare        cap off / unused


━━━━━━━━━━━━━━━━━━━━━━━━━━━━  NOTES  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  1. Commission in order: air handler CN port FIRST (Phase 1), then thermostats.
     Never connect thermostats before CN port comms are verified.

  2. Measure CN port pins with a multimeter before connecting anything.
     5V VCC is present — do not connect it to ESP32.

  3. The voltage divider (10kΩ + 20kΩ) is safety-critical.
     Without it, 5V on GPIO 18 will damage the ESP32-S3.

  4. RS485 termination: for cable runs over ~10 m, add a 120Ω resistor
     between A and B at the far end (thermostat end) of each cable.

  5. All MAX485 modules must be powered from 3.3V (not 5V) to match
     ESP32-S3 logic levels.

  6. Relay board may require active-LOW trigger depending on module type.
     If dampers behave inverted, set IN active-LOW in config.h.
```
