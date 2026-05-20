#pragma once
// =============================================================================
// config.h — All user-configurable parameters
// Edit this file to match your installation.
// =============================================================================

// ── Startup phase ─────────────────────────────────────────────────────────────
// PHASE_LISTEN  (1): RX-only on CN port. Watch for 7E 7E packets.
// PHASE_POLL    (2): Send CMD_POLL. Verify AH responds with 0x2F status.
// PHASE_CONTROL (3): Full zone control active.
#define STARTUP_PHASE           PHASE_LISTEN

// Auto-advance timeouts (ms). Set to 0 to require manual 'N' command.
#define PHASE1_AUTO_ADVANCE_MS  60000
#define PHASE2_AUTO_ADVANCE_MS  30000

// ── Pin assignments ───────────────────────────────────────────────────────────

// CN Port — Air Handler UART (HardwareSerial UART1, 5V logic)
// Use voltage divider (10kΩ + 20kΩ) on CN_RX_PIN: 5V → 3.3V
#define CN_RX_PIN       18
#define CN_TX_PIN       17

// Zone 1 RS485 — HardwareSerial UART2
#define Z1_RX_PIN       16
#define Z1_TX_PIN       15
#define Z1_DE_PIN       14    // MAX485 DE+RE tied together

// Zone 2 RS485 — HardwareSerial UART0 remapped to Z2 pins (full 8E1 support)
// REQUIRED: set "USB CDC On Boot" = Enabled in Arduino IDE board settings,
// or compile with: --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc
// This moves Serial (debug) to native USB-CDC and frees UART0 for Zone 2.
#define Z2_RX_PIN       12
#define Z2_TX_PIN       11
#define Z2_DE_PIN       13

// Zone 3 RS485 — SoftwareSerial (8N1 only — 8E1 parity NOT supported)
// Zone 3 thermostat communication will fail due to parity framing errors.
// Fix: replace with SC16IS752 I2C dual-UART expander (~$8, shares I2C bus).
// See COMMISSIONING.md §10 for wiring details.
#define Z3_RX_PIN        9
#define Z3_TX_PIN        8
#define Z3_DE_PIN       10

// Damper relays (active HIGH = damper open, LOW = spring-return closed)
#define Z1_RELAY_PIN     4
#define Z2_RELAY_PIN     5
#define Z3_RELAY_PIN     6

// SDP810 pressure sensor — I2C (uses default Wire pins on ESP32-S3)
// Default ESP32-S3 I2C: SDA=GPIO8, SCL=GPIO9
// If those conflict with Z3 pins above, override here:
#define PRESSURE_SDA_PIN   38    // Use alternate I2C pins
#define PRESSURE_SCL_PIN   39

// Status LED
#define STATUS_LED_PIN     2

// ── Pressure control (SDP810-500Pa) ──────────────────────────────────────────

// Target static pressure in the supply plenum (Pascals)
// Typical residential: 50–125 Pa (0.20–0.50" WC)
// Start at 62 Pa (0.25" WC) and adjust based on your duct design.
// Use serial command 'P+' / 'P-' to tune at runtime.
#define TARGET_PRESSURE_PA      62.0f   // 0.25" WC

// Pressure hysteresis bands for fan speed switching (Pa)
// Prevents rapid fan speed oscillation near step boundaries
#define PRESSURE_HYST_PA         8.0f

// PID tuning — conservative defaults; tune on site
// Increase Kp if fan responds too slowly to pressure changes
// Increase Ki if there is persistent steady-state pressure error
// Increase Kd if fan speed oscillates
#define PID_KP                   0.8f
#define PID_KI                   0.08f
#define PID_KD                   0.04f

// PID integral windup clamp (Pa)
#define PID_INTEGRAL_MAX        200.0f

// Filter alert threshold: if pressure rises this many Pa above target
// at the same fan speed, the filter is likely loaded. Alert via MQTT.
#define FILTER_ALERT_DELTA_PA   30.0f

// Minimum Pa reading to consider the system running (sanity check)
#define MIN_OPERATING_PRESSURE_PA  10.0f

// SDP810-500Pa scale factor (LSB per Pa, from Sensirion datasheet)
#define SDP810_SCALE_FACTOR     240     // 500Pa model = 240

// ── Timing ────────────────────────────────────────────────────────────────────

// How often to send 0x2C command to air handler (ms)
#define CTRL_INTERVAL_MS        300

// Thermostat poll response timeout — how long to wait for our 0x2F
// response to be transmitted before switching MAX485 back to RX (ms)
#define STAT_RESPONSE_TIMEOUT_MS  20

// Zone thermostat comm timeout — zone forced off if silent (ms)
#define ZONE_TIMEOUT_MS         3000

// Air handler comm timeout (ms)
#define AH_TIMEOUT_MS           5000

// Pressure sensor read interval (ms)
#define PRESSURE_READ_MS        100

// MQTT publish interval (ms)
#define MQTT_INTERVAL_MS        5000

// Minimum interval between MQTT reconnect attempts (ms)
#define MQTT_RECONNECT_MS       5000

// ── Anti-short-cycle protection ───────────────────────────────────────────────

// Minimum time system must run before it can turn off (ms)
// Protects compressor from rapid cycling
#define MIN_ON_TIME_MS          (3 * 60 * 1000)    // 3 minutes

// Minimum off time before system can turn back on (ms)
#define MIN_OFF_TIME_MS         (3 * 60 * 1000)    // 3 minutes

// ── Commissioning flags ───────────────────────────────────────────────────────

// Set to true after Phase 2 commissioning confirms PKT_OFF_ROOM_TEMP is correct.
// When false, a 0x00 encoded byte at that position falls back to AH indoor sensor
// (prevents misreading 16°C as "byte not present" after commissioning is complete).
#define ROOM_TEMP_BYTE_VERIFIED  false

// ── Temperature & zone ────────────────────────────────────────────────────────

// Temperature byte encoding in Gree 0x2C packets
// Byte = (setpoint_C - 16) << 4   range 16–30°C
#define TEMP_ENCODE(c)   ((uint8_t)(((int)(c) - 16) << 4))
#define TEMP_DECODE(b)   ((uint8_t)(((b) >> 4) + 16))

// Indoor temp from 0x2F response byte 46: value - 40 = °C
#define INDOOR_TEMP_DECODE(b)   ((int8_t)((int)(b) - 40))

// Default setpoint if thermostat hasn't sent one yet
#define DEFAULT_SETPOINT_C      20

// ── MQTT ──────────────────────────────────────────────────────────────────────
#define MQTT_TOPIC_PREFIX       "gree_zone"
#define MQTT_CLIENT_ID          "gree_zone_ctrl"
#define MQTT_PORT               1883
