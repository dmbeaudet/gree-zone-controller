// =============================================================================
// GreeZoneController.ino — v0.2
// =============================================================================
// Custom 3-zone controller for Gree FXE48HP230V1R32AH
// Replaces Emerson EMM-3. Retains full Gree RS485 serial protocol.
//
// v0.2 additions over v0.1:
//   - SDP810-500Pa duct pressure sensor (I2C)
//   - PID-based blower speed control (replaces discrete zone-count table)
//   - Virtual room temperature (worst-case zone delta → outdoor inverter demand)
//   - Thermostat slave response (ESP32 sends 0x2F reply to each WK-010WC1)
//   - Anti-short-cycle protection (configurable min on/off timers)
//   - Filter monitoring with MQTT alert
//   - Runtime target pressure adjustment via serial commands P+/P-
//
// Hardware:
//   ESP32-S3 DevKit, 3x MAX485, SDP810-500Pa (I2C),
//   3-ch relay board, 24VAC transformer, 24V→5V buck
//
// Libraries (Arduino Library Manager):
//   PubSubClient, EspSoftwareSerial
// =============================================================================

#include <Arduino.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include "config.h"
#include "protocol.h"
#include "pressure.h"
#include "secrets.h"

// =============================================================================
// BOOT PHASE CONSTANTS
// =============================================================================
#define PHASE_LISTEN   1
#define PHASE_POLL     2
#define PHASE_CONTROL  3

// =============================================================================
// SERIAL PORTS
// =============================================================================
HardwareSerial SerialCN(1);              // UART1 → CN port (air handler)
HardwareSerial SerialZ1(2);             // UART2 → Zone 1 thermostat RS485
SoftwareSerial SerialZ2(Z2_RX_PIN, Z2_TX_PIN);
SoftwareSerial SerialZ3(Z3_RX_PIN, Z3_TX_PIN);

// =============================================================================
// GLOBAL STATE
// =============================================================================
ZoneState zones[3];
AHState   ah;

SDP810         pressureSensor;
PressurePID    pressurePID;
FilterMonitor  filterMonitor;

uint8_t  phase         = STARTUP_PHASE;
uint32_t phaseStartMs  = 0;

// Anti-short-cycle
SystemState sysState        = SystemState::Off;
uint32_t    sysStateMs      = 0;    // when did we enter this state
bool        initSent        = false;

// Timers
uint32_t lastCtrlMs     = 0;
uint32_t lastPressureMs = 0;
uint32_t lastMqttMs     = 0;

// Packet parsers
Parser parserCN, parserZ1, parserZ2, parserZ3;

// WiFi / MQTT
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// Current commanded fan speed step (for filter monitor: 0=off,1=low,2=med,3=high)
uint8_t currentFanStep = 0;

// Runtime-adjustable target pressure (can be tweaked via serial 'P+' / 'P-')
float runtimeTargetPa = TARGET_PRESSURE_PA;

// =============================================================================
// DAMPER CONTROL
// =============================================================================
// Spring-return motors: relay HIGH=open, LOW=spring-closed (fail-safe)
void setDamper(uint8_t zone, DamperState target) {
    if (zones[zone].damper == target) return;
    const int pins[3] = { Z1_RELAY_PIN, Z2_RELAY_PIN, Z3_RELAY_PIN };
    digitalWrite(pins[zone], target == DamperState::Open ? HIGH : LOW);
    zones[zone].damper = target;
    Serial.printf("[DAMPER] Zone %d → %s\n",
                  zone + 1, target == DamperState::Open ? "OPEN" : "CLOSED");
}

void closeAllDampers() {
    for (uint8_t z = 0; z < 3; z++) setDamper(z, DamperState::Closed);
}

// =============================================================================
// SEND TO AIR HANDLER (CN port)
// =============================================================================
void sendToAH(const uint8_t* pkt, uint8_t len) {
    SerialCN.write(pkt, len);
    SerialCN.flush();
    if (phase <= PHASE_POLL) {
        Serial.print("[CN TX] ");
        for (uint8_t i = 0; i < len; i++) Serial.printf("%02X ", pkt[i]);
        Serial.println();
    }
}

// =============================================================================
// SEND 0x2F STATUS RESPONSE TO A THERMOSTAT
// The thermostat polls us (as fake air handler) on its RS485 bus.
// We respond with current AH state + zone-specific setpoint & room temp.
// MAX485 DE pin must go HIGH (TX), we send, then LOW (RX) again.
// =============================================================================
void sendThermostatResponse(uint8_t zone) {
    const int dePins[3] = { Z1_DE_PIN, Z2_DE_PIN, Z3_DE_PIN };
    HardwareSerial* hw = (zone == 0) ? &SerialZ1 : nullptr;
    SoftwareSerial* sw = (zone == 1) ? &SerialZ2 : (zone == 2 ? &SerialZ3 : nullptr);

    uint8_t respBuf[64];
    buildStatusResponse(respBuf, ah, zones[zone].setpointC, zones[zone].roomTempC);
    uint8_t respLen = respBuf[2] + 3;   // length byte + 3 overhead bytes

    // Switch MAX485 to transmit
    digitalWrite(dePins[zone], HIGH);
    delayMicroseconds(50);   // DE setup time

    if (hw) { hw->write(respBuf, respLen); hw->flush(); }
    else if (sw) { sw->write(respBuf, respLen); }

    // At 4800 baud: 1 byte ≈ 2.08ms (8E1 = 11 bits/byte)
    // Wait for all bytes to clock out before releasing the bus
    uint32_t txTimeMs = (uint32_t)respLen * 3;   // generous ~3ms/byte margin
    delay(txTimeMs);

    // Switch MAX485 back to receive
    digitalWrite(dePins[zone], LOW);

    if (phase <= PHASE_POLL) {
        Serial.printf("[Z%d TX] 0x2F response (%d bytes)\n", zone + 1, respLen);
    }
}

// =============================================================================
// PROCESS THERMOSTAT PACKET
// Called when a complete, valid packet arrives from a WK-010WC1.
// Decodes zone demand and sends the 0x2F slave response.
// =============================================================================
void processThermostatPkt(uint8_t zone, const GreePkt* pkt) {
    if (!pkt->valid) {
        Serial.printf("[Z%d] Bad checksum (0x%02X length) — skipping\n",
                      zone + 1, pkt->lenByte);
        return;
    }

    zones[zone].lastPktMs = millis();
    zones[zone].commOk    = true;

    if (pkt->lenByte == PKT_LEN_CONTROL) {
        const uint8_t* d = pkt->raw;
        uint8_t cmd      = d[PKT_OFF_CMD];
        uint8_t modeRaw  = d[PKT_OFF_MODE_FAN];
        uint8_t spByte   = d[PKT_OFF_SETPOINT];
        uint8_t rtByte   = d[PKT_OFF_ROOM_TEMP];   // may be 0 until verified

        zones[zone].setpointC   = TEMP_DECODE(spByte);
        zones[zone].lastModeRaw = modeRaw;

        // Decode room temp from thermostat packet.
        // PKT_OFF_ROOM_TEMP (byte 20) is our best guess — verify during Phase 2.
        // If rtByte is 0 (not yet confirmed), fall back to AH indoor sensor.
        if (rtByte != 0x00) {
            zones[zone].roomTempC = TEMP_DECODE(rtByte);
        } else {
            zones[zone].roomTempC = (uint8_t)(ah.indoorTempC);
        }

        // Calculate demand delta (°C, positive = calling for heat)
        zones[zone].deltaC = (int8_t)zones[zone].setpointC
                           - (int8_t)zones[zone].roomTempC;

        // Determine calling state from mode byte
        uint8_t modeNibble = modeRaw & 0xF0;
        if (modeRaw == MODE_OFF || modeRaw == 0x10) {
            zones[zone].calling = false;
            zones[zone].mode    = ZoneMode::Off;
        } else if (modeNibble == 0xC0) {
            zones[zone].calling = (zones[zone].deltaC > 0);
            zones[zone].mode    = ZoneMode::Heat;
        } else if (modeNibble == 0x90) {
            zones[zone].calling = (zones[zone].deltaC < 0);
            zones[zone].mode    = ZoneMode::Cool;
        } else if (modeNibble == 0x80) {
            zones[zone].calling = (abs(zones[zone].deltaC) > 1);
            zones[zone].mode    = ZoneMode::Auto;
        }

        Serial.printf("[Z%d] cmd:0x%02X mode:0x%02X sp:%d°C rt:%d°C Δ:%+d calling:%s\n",
                      zone+1, cmd, modeRaw,
                      zones[zone].setpointC, zones[zone].roomTempC,
                      zones[zone].deltaC,
                      zones[zone].calling ? "YES" : "NO");

        // Phase diagnostics: dump unknown bytes to help find room temp byte
        if (phase == PHASE_POLL) {
            Serial.printf("[Z%d SCAN] bytes 16-25: ", zone+1);
            for (uint8_t i = 16; i <= 25 && i < pkt->totalBytes; i++) {
                Serial.printf("[%d]=0x%02X ", i, pkt->raw[i]);
            }
            Serial.println();
        }
    }

    // Always respond to thermostat (fake AH slave response)
    // This prevents the thermostat from showing a communication error
    sendThermostatResponse(zone);
}

// =============================================================================
// VIRTUAL ROOM TEMPERATURE CALCULATION
// Find the active zone with the highest demand (largest delta).
// Synthesise a virtual room temp = commandedSetpoint - maxDelta.
// This is what we send to the air handler, telling the outdoor inverter
// exactly how hard to work based on the worst-case zone.
// =============================================================================
struct ZoneAggregation {
    uint8_t activeCount     = 0;
    uint8_t maxSetpointC    = DEFAULT_SETPOINT_C;
    int8_t  maxDeltaC       = 0;
    uint8_t virtualRoomC    = DEFAULT_SETPOINT_C;
    ZoneMode dominantMode   = ZoneMode::Off;
};

ZoneAggregation aggregateZones() {
    ZoneAggregation agg;
    for (uint8_t z = 0; z < 3; z++) {
        if (!zones[z].calling || !zones[z].commOk) continue;
        agg.activeCount++;
        if (zones[z].setpointC > agg.maxSetpointC) {
            agg.maxSetpointC = zones[z].setpointC;
        }
        if (abs(zones[z].deltaC) > abs(agg.maxDeltaC)) {
            agg.maxDeltaC   = zones[z].deltaC;
            agg.dominantMode = zones[z].mode;
        }
    }
    if (agg.activeCount > 0) {
        // Virtual room temp = highest setpoint minus worst-case delta
        // Clamp to valid Gree range (16–30°C)
        int virt = (int)agg.maxSetpointC - (int)agg.maxDeltaC;
        agg.virtualRoomC = (uint8_t)constrain(virt, 16, 30);
    }
    return agg;
}

// =============================================================================
// ANTI-SHORT-CYCLE STATE MACHINE
// Manages transitions between Off → MinOffHold → Running → MinOnHold → Off
// =============================================================================
bool shortCycleAllowsStart() {
    if (sysState == SystemState::Off) return true;
    if (sysState == SystemState::MinOffHold) {
        if (millis() - sysStateMs >= MIN_OFF_TIME_MS) {
            sysState   = SystemState::Off;
            sysStateMs = millis();
            Serial.println("[ASC] Min-off complete → Ready to start");
            return true;
        }
        return false;
    }
    return true;  // Running or MinOnHold — already on
}

bool shortCycleAllowsStop() {
    if (sysState == SystemState::Running) {
        if (millis() - sysStateMs < MIN_ON_TIME_MS) {
            // Still in minimum-on period — keep running
            uint32_t remaining = MIN_ON_TIME_MS - (millis() - sysStateMs);
            Serial.printf("[ASC] Min-on hold: %lu s remaining\n", remaining / 1000);
            return false;
        }
    }
    return true;
}

void transitionToRunning() {
    if (sysState != SystemState::Running) {
        sysState   = SystemState::Running;
        sysStateMs = millis();
        Serial.println("[ASC] System ON (min-on timer started)");
    }
}

void transitionToOff() {
    if (sysState == SystemState::Running || sysState == SystemState::Off) {
        sysState   = (sysState == SystemState::Running)
                     ? SystemState::MinOffHold : SystemState::Off;
        sysStateMs = millis();
        if (sysState == SystemState::MinOffHold) {
            Serial.println("[ASC] System OFF (min-off hold started)");
        }
    }
}

// =============================================================================
// MAIN CONTROL UPDATE
// Called every CTRL_INTERVAL_MS in PHASE_CONTROL.
// =============================================================================
void updateControl() {
    ZoneAggregation agg = aggregateZones();
    bool anyActive = (agg.activeCount > 0);

    // Open/close dampers per zone
    for (uint8_t z = 0; z < 3; z++) {
        bool active = zones[z].calling && zones[z].commOk;
        setDamper(z, active ? DamperState::Open : DamperState::Closed);
    }

    uint8_t pkt[CTRL_PKT_TOTAL];

    if (!anyActive) {
        // All zones satisfied — check if we can stop
        if (shortCycleAllowsStop()) {
            transitionToOff();
            buildControlPkt(pkt, CMD_SET, MODE_OFF, DEFAULT_SETPOINT_C, 0xFF);
            currentFanStep = 0;
            Serial.println("[CTRL] → OFF");
        } else {
            // Must keep running through min-on period with lowest fan
            bool isHeat = (agg.dominantMode != ZoneMode::Cool);
            uint8_t mf = isHeat ? MODE_HEAT_LOW : MODE_COOL_LOW;
            buildControlPkt(pkt, CMD_SET, mf, DEFAULT_SETPOINT_C,
                            agg.virtualRoomC);
            currentFanStep = 1;
        }
    } else {
        // Zones calling — check anti-short-cycle before starting
        if (!shortCycleAllowsStart()) {
            // In min-off hold — can't start yet. Close dampers and wait.
            closeAllDampers();
            buildControlPkt(pkt, CMD_SET, MODE_OFF, DEFAULT_SETPOINT_C, 0xFF);
            sendToAH(pkt, CTRL_PKT_TOTAL);
            currentFanStep = 0;
            return;
        }
        transitionToRunning();

        bool isHeat = (agg.dominantMode != ZoneMode::Cool);

        // ── Fan speed via PID pressure control ────────────────────────────
        // If pressure sensor is healthy, use PID output.
        // If sensor failed, fall back to zone-count heuristic.
        uint8_t modeAndFan;

        if (pressureSensor.isAlive()) {
            pressurePID.targetPa = runtimeTargetPa;
            pressurePID.update(pressureSensor.pressurePa);
            modeAndFan = pressurePID.toFanByte(isHeat, pressureSensor.pressurePa);

            // Update filter monitor
            // Determine current speed step from fan byte
            uint8_t fanNibble = modeAndFan & 0x0F;
            currentFanStep = (fanNibble == 1) ? 1 : (fanNibble == 2) ? 2 : 3;
            filterMonitor.update(currentFanStep, pressureSensor.pressurePa);

        } else {
            // Pressure sensor fallback: zone-count heuristic
            Serial.println("[CTRL] Pressure sensor offline — using zone-count fallback");
            if (isHeat) {
                modeAndFan = (agg.activeCount == 1) ? MODE_HEAT_LOW :
                             (agg.activeCount == 2) ? MODE_HEAT_MED : MODE_HEAT_HIGH;
            } else {
                modeAndFan = (agg.activeCount == 1) ? MODE_COOL_LOW :
                             (agg.activeCount == 2) ? MODE_COOL_MED : MODE_COOL_HIGH;
            }
            currentFanStep = agg.activeCount;
        }

        buildControlPkt(pkt, CMD_SET, modeAndFan,
                        agg.maxSetpointC, agg.virtualRoomC);

        Serial.printf("[CTRL] zones:%d sp:%d°C vRoom:%d°C Δ:%+d fan:0x%02X "
                      "P:%.1fPa target:%.1fPa PID:%.1f\n",
                      agg.activeCount, agg.maxSetpointC, agg.virtualRoomC,
                      agg.maxDeltaC, modeAndFan,
                      pressureSensor.pressurePa, runtimeTargetPa,
                      pressurePID.output);
    }

    sendToAH(pkt, CTRL_PKT_TOTAL);
}

// =============================================================================
// WIFI & MQTT
// =============================================================================
void setupWifi() {
    Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
        delay(250); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WIFI] No connection — standalone mode");
    }
}

void mqttPublish() {
    if (!mqtt.connected()) return;
    char topic[96], val[32];

    for (uint8_t z = 0; z < 3; z++) {
        auto pub = [&](const char* suffix, const char* v) {
            snprintf(topic, sizeof(topic), "%s/zone%d/%s", MQTT_TOPIC_PREFIX, z+1, suffix);
            mqtt.publish(topic, v, true);
        };
        pub("calling",     zones[z].calling ? "1" : "0");
        snprintf(val, sizeof(val), "%d", zones[z].setpointC);
        pub("setpoint_c",  val);
        snprintf(val, sizeof(val), "%d", zones[z].roomTempC);
        pub("room_temp_c", val);
        snprintf(val, sizeof(val), "%+d", zones[z].deltaC);
        pub("delta_c",     val);
        pub("damper",      zones[z].damper == DamperState::Open ? "open" : "closed");
        pub("comm_ok",     zones[z].commOk ? "1" : "0");
    }

    auto pub = [&](const char* suffix, const char* v) {
        snprintf(topic, sizeof(topic), "%s/airhandler/%s", MQTT_TOPIC_PREFIX, suffix);
        mqtt.publish(topic, v, true);
    };
    pub("powered",      ah.powered ? "1" : "0");
    snprintf(val, sizeof(val), "%d",    ah.indoorTempC); pub("indoor_temp_c", val);
    snprintf(val, sizeof(val), "%d",    ah.setpointC);   pub("setpoint_c",    val);
    snprintf(val, sizeof(val), "0x%02X",ah.modeRaw);     pub("mode_raw",      val);
    pub("comm_ok",      ah.commOk ? "1" : "0");

    // Pressure
    snprintf(topic, sizeof(topic), "%s/pressure/pa",      MQTT_TOPIC_PREFIX);
    snprintf(val,   sizeof(val),   "%.1f", pressureSensor.pressurePa);
    mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%s/pressure/target_pa", MQTT_TOPIC_PREFIX);
    snprintf(val,   sizeof(val),   "%.1f", runtimeTargetPa);
    mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%s/pressure/sensor_ok", MQTT_TOPIC_PREFIX);
    mqtt.publish(topic, pressureSensor.isAlive() ? "1" : "0", true);

    snprintf(topic, sizeof(topic), "%s/pressure/pid_output", MQTT_TOPIC_PREFIX);
    snprintf(val,   sizeof(val),   "%.2f", pressurePID.output);
    mqtt.publish(topic, val, true);

    // Filter alert
    snprintf(topic, sizeof(topic), "%s/filter/alert", MQTT_TOPIC_PREFIX);
    mqtt.publish(topic, filterMonitor.alertActive ? "1" : "0", true);

    // System state
    snprintf(topic, sizeof(topic), "%s/system/state", MQTT_TOPIC_PREFIX);
    const char* stateStr = (sysState == SystemState::Off)        ? "off"         :
                           (sysState == SystemState::MinOffHold)  ? "min_off_hold":
                           (sysState == SystemState::Running)     ? "running"     :
                                                                    "min_on_hold";
    mqtt.publish(topic, stateStr, true);

    snprintf(topic, sizeof(topic), "%s/system/phase", MQTT_TOPIC_PREFIX);
    snprintf(val, sizeof(val), "%d", phase);
    mqtt.publish(topic, val, true);
}

// =============================================================================
// SERIAL COMMAND HANDLER
// =============================================================================
void printHelp() {
    Serial.println("=== Commands ===");
    Serial.println("  N       Next phase");
    Serial.println("  S       Status snapshot");
    Serial.println("  P+/P-   Increase/decrease target pressure by 5 Pa");
    Serial.println("  R       Reset PID integrator");
    Serial.println("  1/2/3   Force zone ON (override)");
    Serial.println("  0       Force all zones OFF");
    Serial.println("  H       This help");
}

void handleSerialCmd(char c) {
    switch (c) {
        case 'N':
            if (phase < PHASE_CONTROL) {
                phase++; phaseStartMs = millis();
                Serial.printf("[CMD] Phase → %d\n", phase);
            } else Serial.println("[CMD] Already Phase 3");
            break;
        case 'S': {
            Serial.println("=== STATUS ===");
            Serial.printf("Phase: %d  SysState: %d  PID target: %.1f Pa\n",
                          phase, (int)sysState, runtimeTargetPa);
            Serial.printf("Pressure: %.1f Pa  Sensor: %s  Filter alert: %s\n",
                          pressureSensor.pressurePa,
                          pressureSensor.isAlive() ? "OK" : "FAIL",
                          filterMonitor.alertActive ? "YES" : "no");
            Serial.printf("AH: pwr=%s indoor=%d°C mode=0x%02X commOk=%s\n",
                          ah.powered?"Y":"N", ah.indoorTempC,
                          ah.modeRaw, ah.commOk?"Y":"N");
            for (uint8_t z = 0; z < 3; z++) {
                Serial.printf("Z%d: call=%s sp=%d rt=%d Δ=%+d damper=%s commOk=%s\n",
                    z+1, zones[z].calling?"Y":"N",
                    zones[z].setpointC, zones[z].roomTempC, zones[z].deltaC,
                    zones[z].damper==DamperState::Open?"open":"closed",
                    zones[z].commOk?"Y":"N");
            }
            break;
        }
        case 'P':
            // Look ahead for + or -
            delay(20);
            if (Serial.available()) {
                char c2 = Serial.read();
                if (c2 == '+') {
                    runtimeTargetPa += 5.0f;
                    Serial.printf("[CMD] Target pressure → %.1f Pa\n", runtimeTargetPa);
                } else if (c2 == '-') {
                    runtimeTargetPa = max(10.0f, runtimeTargetPa - 5.0f);
                    Serial.printf("[CMD] Target pressure → %.1f Pa\n", runtimeTargetPa);
                }
            }
            break;
        case 'R':
            pressurePID.reset();
            Serial.println("[CMD] PID reset");
            break;
        case '1': zones[0].calling=true;  zones[0].commOk=true; Serial.println("[CMD] Z1 ON"); break;
        case '2': zones[1].calling=true;  zones[1].commOk=true; Serial.println("[CMD] Z2 ON"); break;
        case '3': zones[2].calling=true;  zones[2].commOk=true; Serial.println("[CMD] Z3 ON"); break;
        case '0':
            for (uint8_t z=0;z<3;z++) zones[z].calling=false;
            Serial.println("[CMD] All zones OFF");
            break;
        case 'H': printHelp(); break;
        default: break;
    }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=============================================");
    Serial.println("  Gree Zone Controller  v0.2");
    Serial.println("  github.com/dmbeaudet/gree-zone-controller");
    Serial.println("=============================================");
    Serial.printf("  Phase: %d   Target pressure: %.1f Pa\n\n",
                  STARTUP_PHASE, TARGET_PRESSURE_PA);
    printHelp();
    Serial.println();

    // CN port — air handler (5V logic, RX via voltage divider)
    SerialCN.begin(4800, SERIAL_8E1, CN_RX_PIN, CN_TX_PIN);

    // Zone 1 RS485
    SerialZ1.begin(4800, SERIAL_8E1, Z1_RX_PIN, Z1_TX_PIN);
    pinMode(Z1_DE_PIN, OUTPUT); digitalWrite(Z1_DE_PIN, LOW);

    // Zone 2 + 3 RS485 (SoftwareSerial — no parity, see config.h note)
    SerialZ2.begin(4800);
    SerialZ3.begin(4800);
    pinMode(Z2_DE_PIN, OUTPUT); digitalWrite(Z2_DE_PIN, LOW);
    pinMode(Z3_DE_PIN, OUTPUT); digitalWrite(Z3_DE_PIN, LOW);

    // Relay outputs — all closed (safe state)
    pinMode(Z1_RELAY_PIN, OUTPUT); digitalWrite(Z1_RELAY_PIN, LOW);
    pinMode(Z2_RELAY_PIN, OUTPUT); digitalWrite(Z2_RELAY_PIN, LOW);
    pinMode(Z3_RELAY_PIN, OUTPUT); digitalWrite(Z3_RELAY_PIN, LOW);

    // Status LED
    pinMode(STATUS_LED_PIN, OUTPUT); digitalWrite(STATUS_LED_PIN, LOW);

    // SDP810 pressure sensor
    Wire.begin(PRESSURE_SDA_PIN, PRESSURE_SCL_PIN);
    if (!pressureSensor.begin(Wire)) {
        Serial.println("[BOOT] WARNING: Pressure sensor not found — "
                       "will use zone-count fallback for fan speed");
    }
    pressurePID.targetPa = TARGET_PRESSURE_PA;
    runtimeTargetPa      = TARGET_PRESSURE_PA;

    // Init state
    for (uint8_t z = 0; z < 3; z++) zones[z] = ZoneState();
    memset(&ah, 0, sizeof(ah));
    ah.setpointC   = DEFAULT_SETPOINT_C;
    ah.indoorTempC = DEFAULT_SETPOINT_C;

    setupWifi();
    mqtt.setServer(MQTT_BROKER_IP, MQTT_PORT);

    phase        = STARTUP_PHASE;
    phaseStartMs = millis();
    sysState     = SystemState::Off;
    sysStateMs   = millis();

    // Send one-time handshake to air handler (safe even in listen mode —
    // we just won't transmit it until Phase 2)
    Serial.printf("[BOOT] Phase %d active\n", phase);
    if (phase == PHASE_LISTEN)
        Serial.println("[BOOT] RX-only: watching for 7E 7E packets on CN port...");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    uint32_t now = millis();

    // WiFi / MQTT maintenance
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqtt.connected()) mqtt.connect(MQTT_CLIENT_ID);
        mqtt.loop();
    }

    // Serial commands
    if (Serial.available()) handleSerialCmd((char)Serial.read());

    // Phase auto-advance
    if (phase == PHASE_LISTEN && PHASE1_AUTO_ADVANCE_MS > 0 &&
        now - phaseStartMs >= PHASE1_AUTO_ADVANCE_MS) {
        Serial.println("[PHASE] Listen → Poll");
        phase = PHASE_POLL; phaseStartMs = now;
    }
    if (phase == PHASE_POLL && PHASE2_AUTO_ADVANCE_MS > 0 &&
        now - phaseStartMs >= PHASE2_AUTO_ADVANCE_MS) {
        Serial.println("[PHASE] Poll → Control");
        phase = PHASE_CONTROL; phaseStartMs = now;
        // Send init handshake once when entering control
        if (!initSent) {
            uint8_t initPkt[INIT_PKT_TOTAL];
            buildInitPkt(initPkt);
            sendToAH(initPkt, INIT_PKT_TOTAL);
            initSent = true;
            delay(100);
        }
    }

    // ── Read CN port (Air Handler → ESP32) ───────────────────────────────────
    while (SerialCN.available()) {
        GreePkt pkt;
        if (parserFeed(&parserCN, (uint8_t)SerialCN.read(), &pkt)) {
            if (phase <= PHASE_POLL) dumpPkt("CN RX", &pkt);
            parseAHStatus(&pkt, &ah);
        }
    }

    // ── Read Zone thermostats ─────────────────────────────────────────────────
    while (SerialZ1.available()) {
        GreePkt pkt;
        if (parserFeed(&parserZ1, (uint8_t)SerialZ1.read(), &pkt))
            processThermostatPkt(0, &pkt);
    }
    while (SerialZ2.available()) {
        GreePkt pkt;
        if (parserFeed(&parserZ2, (uint8_t)SerialZ2.read(), &pkt))
            processThermostatPkt(1, &pkt);
    }
    while (SerialZ3.available()) {
        GreePkt pkt;
        if (parserFeed(&parserZ3, (uint8_t)SerialZ3.read(), &pkt))
            processThermostatPkt(2, &pkt);
    }

    // ── Pressure sensor ───────────────────────────────────────────────────────
    if (now - lastPressureMs >= PRESSURE_READ_MS) {
        lastPressureMs = now;
        pressureSensor.update();
    }

    // ── Main control ──────────────────────────────────────────────────────────
    if (now - lastCtrlMs >= CTRL_INTERVAL_MS) {
        lastCtrlMs = now;
        uint8_t pkt[CTRL_PKT_TOTAL];

        if (phase == PHASE_LISTEN) {
            // RX only — no transmit
        } else if (phase == PHASE_POLL) {
            buildControlPkt(pkt, CMD_POLL, MODE_HEAT_AUTO,
                            DEFAULT_SETPOINT_C, 0xFF);
            sendToAH(pkt, CTRL_PKT_TOTAL);
        } else {
            updateControl();
        }
    }

    // ── Communication timeouts ────────────────────────────────────────────────
    for (uint8_t z = 0; z < 3; z++) {
        if (zones[z].commOk && zones[z].lastPktMs > 0 &&
            now - zones[z].lastPktMs > ZONE_TIMEOUT_MS) {
            zones[z].commOk  = false;
            zones[z].calling = false;
            Serial.printf("[WARN] Zone %d timeout — forced OFF\n", z+1);
        }
    }
    if (ah.commOk && ah.lastPktMs > 0 &&
        now - ah.lastPktMs > AH_TIMEOUT_MS) {
        ah.commOk = false;
        Serial.println("[WARN] Air handler timeout");
    }

    // ── MQTT publish ──────────────────────────────────────────────────────────
    if (now - lastMqttMs >= MQTT_INTERVAL_MS) {
        lastMqttMs = now;
        mqttPublish();
    }

    // ── Status LED ────────────────────────────────────────────────────────────
    static uint32_t ledMs = 0;
    static bool ledOn = false;
    uint32_t blinkMs;
    if      (phase == PHASE_LISTEN)  blinkMs = 1000;
    else if (phase == PHASE_POLL)    blinkMs = 500;
    else if (sysState == SystemState::Running) blinkMs = 200;
    else blinkMs = 0;  // steady on

    if (blinkMs == 0) {
        digitalWrite(STATUS_LED_PIN, HIGH);
    } else if (now - ledMs >= blinkMs) {
        ledMs = now;
        ledOn = !ledOn;
        digitalWrite(STATUS_LED_PIN, ledOn);
    }
}
