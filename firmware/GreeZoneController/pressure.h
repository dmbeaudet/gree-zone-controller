#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "protocol.h"   // for MODE_HEAT_* / MODE_COOL_* constants

// =============================================================================
// pressure.h — Sensirion SDP810-500Pa driver + PID blower speed controller
//
// Wiring:
//   SDP810 VDD  → 3.3V
//   SDP810 GND  → GND
//   SDP810 SDA  → PRESSURE_SDA_PIN (with 4.7kΩ pull-up to 3.3V)
//   SDP810 SCL  → PRESSURE_SCL_PIN (with 4.7kΩ pull-up to 3.3V)
//   SDP810 tube → Supply plenum (high pressure port)
//   SDP810 open → Room ambient (low pressure port — leave open to air)
//
// I2C address: 0x25 (both 500Pa and 125Pa models share this address)
// =============================================================================

#define SDP810_I2C_ADDR         0x25

// I2C commands (16-bit, MSB first)
// Continuous measurement, differential pressure mode, with temp compensation
#define SDP810_CMD_START_CONT   0x3615
#define SDP810_CMD_STOP         0x3FF9
#define SDP810_CMD_RESET        0x0006

// =============================================================================
// CRC-8 verification (Sensirion polynomial: 0x31, init: 0xFF)
// =============================================================================
static uint8_t sdp810_crc(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
        }
    }
    return crc;
}

// =============================================================================
// SDP810 driver class
// =============================================================================
class SDP810 {
public:
    float    pressurePa     = 0.0f;   // latest reading (Pa)
    float    temperatureC   = 0.0f;   // sensor die temperature
    bool     healthy        = false;  // false if no valid reading in last 2s
    uint32_t lastReadMs     = 0;

    bool begin(TwoWire& wire = Wire) {
        _wire = &wire;

        // Soft reset
        _wire->beginTransmission(SDP810_I2C_ADDR);
        _wire->write(SDP810_CMD_RESET >> 8);
        _wire->write(SDP810_CMD_RESET & 0xFF);
        _wire->endTransmission();
        delay(25);   // 25ms power-up per datasheet

        // Start continuous measurement
        _wire->beginTransmission(SDP810_I2C_ADDR);
        _wire->write(SDP810_CMD_START_CONT >> 8);
        _wire->write(SDP810_CMD_START_CONT & 0xFF);
        uint8_t err = _wire->endTransmission();

        if (err != 0) {
            Serial.printf("[SDP810] Init failed, I2C error %d\n", err);
            return false;
        }
        delay(50);   // First measurement available after 50ms
        Serial.println("[SDP810] Initialized OK, continuous measurement started");
        return true;
    }

    // Call this every PRESSURE_READ_MS. Returns true if reading is valid.
    bool update() {
        uint8_t buf[6];
        uint8_t rxLen = _wire->requestFrom((uint8_t)SDP810_I2C_ADDR, (uint8_t)6);
        if (rxLen != 6) {
            Serial.printf("[SDP810] Short read: got %d bytes\n", rxLen);
            healthy = false;
            return false;
        }
        for (uint8_t i = 0; i < 6; i++) buf[i] = _wire->read();

        // Verify pressure CRC (bytes 0,1 → CRC byte 2)
        if (sdp810_crc(buf, 2) != buf[2]) {
            Serial.println("[SDP810] Pressure CRC fail");
            healthy = false;
            return false;
        }
        // Verify temperature CRC (bytes 3,4 → CRC byte 5)
        if (sdp810_crc(buf + 3, 2) != buf[5]) {
            Serial.println("[SDP810] Temp CRC fail");
            healthy = false;
            return false;
        }

        int16_t rawPressure = (int16_t)((buf[0] << 8) | buf[1]);
        int16_t rawTemp     = (int16_t)((buf[3] << 8) | buf[4]);

        pressurePa   = (float)rawPressure / SDP810_SCALE_FACTOR;
        temperatureC = (float)rawTemp / 200.0f;

        // Clamp negative pressure to 0 (shouldn't happen in supply plenum)
        if (pressurePa < 0.0f) pressurePa = 0.0f;

        healthy   = true;
        lastReadMs = millis();
        return true;
    }

    // Returns true if sensor has produced a valid reading within 2 seconds
    bool isAlive() {
        return healthy && (millis() - lastReadMs < 2000);
    }

private:
    TwoWire* _wire = nullptr;
};

// =============================================================================
// PID controller for blower pressure
// =============================================================================
class PressurePID {
public:
    float    targetPa       = TARGET_PRESSURE_PA;
    float    kp             = PID_KP;
    float    ki             = PID_KI;
    float    kd             = PID_KD;
    float    output         = 0.0f;   // continuous PID output
    float    lastError      = 0.0f;
    float    integral       = 0.0f;
    uint32_t lastUpdateMs   = 0;

    void reset() {
        integral      = 0.0f;
        lastError     = 0.0f;
        output        = 0.0f;
        lastUpdateMs  = 0;
    }

    // Update PID with new pressure reading. Returns output (positive = need more fan).
    // output > 0: measured pressure too low → increase fan
    // output < 0: measured pressure too high → reduce fan
    float update(float measuredPa) {
        uint32_t now = millis();
        float dt = (lastUpdateMs == 0) ? 0.1f :
                   (float)(now - lastUpdateMs) / 1000.0f;
        lastUpdateMs = now;
        if (dt <= 0.0f) dt = 0.001f;

        float error = targetPa - measuredPa;

        // Anti-windup: only integrate if not saturated
        integral += error * dt;
        integral = constrain(integral, -PID_INTEGRAL_MAX, PID_INTEGRAL_MAX);

        float derivative = (error - lastError) / dt;
        lastError = error;

        output = kp * error + ki * integral + kd * derivative;
        return output;
    }

    // Map PID output to a Gree mode+fan byte.
    // isHeat: true = heating mode, false = cooling mode.
    // Uses hysteresis to prevent rapid oscillation between speed steps.
    uint8_t toFanByte(bool isHeat, float measuredPa) {
        // Fan step thresholds based on how far measured pressure is from target.
        // output > 0 → pressure too low → fan too slow → step up
        // output < 0 → pressure too high → fan too fast → step down

        // Current speed band with hysteresis
        float H = PRESSURE_HYST_PA;

        // Map measured pressure to fan speed:
        // We target TARGET_PRESSURE_PA.
        // If pressure is well below target → High (fan isn't moving enough air)
        // If pressure is near target       → Med
        // If pressure is well above target → Low (dampers are mostly closed)
        //
        // NOTE: counter-intuitive at first glance —
        //   More dampers open   → lower static pressure → fan should go faster
        //   Fewer dampers open  → higher static pressure → fan should slow down
        //
        // target - measured > H+10 → Low (too much back-pressure, only 1 zone open)
        // target - measured in (-H, H+10) → Med
        // target - measured < -H  → High (lots of zones open, need more airflow)

        // Use a simple bang-bang with hysteresis on the output:
        uint8_t speed;
        if (output < -(H * 1.5f)) {
            speed = 3;   // High
        } else if (output < H) {
            speed = 2;   // Medium (includes near-target band)
        } else {
            speed = 1;   // Low (over-pressure — step down)
        }

        if (isHeat) {
            switch (speed) {
                case 1:  return MODE_HEAT_LOW;
                case 2:  return MODE_HEAT_MED;
                default: return MODE_HEAT_HIGH;
            }
        } else {
            switch (speed) {
                case 1:  return MODE_COOL_LOW;
                case 2:  return MODE_COOL_MED;
                default: return MODE_COOL_HIGH;
            }
        }
    }
};

// =============================================================================
// Filter monitoring
// Tracks baseline pressure at each fan speed. Alerts when pressure rises
// significantly above baseline (loaded filter = increased resistance).
// =============================================================================
class FilterMonitor {
public:
    // Baseline pressure at each speed (Pa). Updated slowly over time.
    float baseline[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // [off, low, med, high]
    bool  alertActive  = false;
    uint8_t currentSpeed = 0;

    void update(uint8_t fanSpeedStep, float pressurePa) {
        // fanSpeedStep: 0=off, 1=low, 2=med, 3=high
        if (fanSpeedStep == 0 || pressurePa < MIN_OPERATING_PRESSURE_PA) return;

        currentSpeed = fanSpeedStep;

        // Slowly update baseline (exponential moving average, τ ≈ 10 minutes)
        float alpha = 0.0001f;  // Very slow learning rate
        if (baseline[fanSpeedStep] < MIN_OPERATING_PRESSURE_PA) {
            baseline[fanSpeedStep] = pressurePa;  // First sample
        } else {
            baseline[fanSpeedStep] = baseline[fanSpeedStep] * (1.0f - alpha)
                                   + pressurePa * alpha;
        }

        // Alert if current pressure exceeds baseline by threshold
        bool alert = (pressurePa - baseline[fanSpeedStep]) > FILTER_ALERT_DELTA_PA;
        if (alert != alertActive) {
            alertActive = alert;
            Serial.printf("[FILTER] Alert %s: pressure=%.1fPa baseline=%.1fPa\n",
                alert ? "ON" : "OFF", pressurePa, baseline[fanSpeedStep]);
        }
    }
};
