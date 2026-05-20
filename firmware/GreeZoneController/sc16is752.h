#pragma once
#include <Arduino.h>
#include <Wire.h>

// =============================================================================
// sc16is752.h — Minimal I2C driver for SC16IS752 dual-UART bridge
// Configured for 4800 8E1 (Gree RS485 thermostat bus, Zone 3)
//
// No external library required — all register access is inline here.
//
// Wiring (I2C mode, channel A):
//   SC16IS752 VCC  → 3.3V
//   SC16IS752 GND  → GND
//   SC16IS752 SDA  → PRESSURE_SDA_PIN / GPIO 38  (shared I2C bus)
//   SC16IS752 SCL  → PRESSURE_SCL_PIN / GPIO 39  (shared I2C bus)
//   SC16IS752 A0   → GND   ──┐ I2C device address = 0x48
//   SC16IS752 A1   → GND   ──┘
//   SC16IS752 TXD  → Z3 MAX485 DI  (data in)
//   SC16IS752 RXD  ← Z3 MAX485 RO  (receiver out)
//   ESP32 Z3_DE_PIN → Z3 MAX485 DE/RE  (manual direction control, unchanged)
//
// I2C addresses (A1:A0):
//   GND:GND = 0x48    GND:VCC = 0x49
//   VCC:GND = 0x4A    VCC:VCC = 0x4B
//
// Baud rate: crystal / (16 × divisor)
//   14.7456 MHz / (16 × 192) = 4800 baud  (divisor = 192, exact)
// =============================================================================

// ── Register addresses (un-shifted — see _reg() for channel encoding) ────────
#define SC16_RHR     0x00   // Receive Holding Register  (R)
#define SC16_THR     0x00   // Transmit Holding Register (W)
#define SC16_IER     0x01   // Interrupt Enable Register
#define SC16_FCR     0x02   // FIFO Control Register     (W)
#define SC16_LCR     0x03   // Line Control Register
#define SC16_MCR     0x04   // Modem Control Register
#define SC16_LSR     0x05   // Line Status Register      (R)
#define SC16_RXLVL   0x09   // RX FIFO level             (R)
// DLL/DLH share addresses 0x00/0x01 when LCR[7]=1 (DLAB mode)
#define SC16_DLL     0x00   // Divisor Latch Low  (DLAB=1)
#define SC16_DLH     0x01   // Divisor Latch High (DLAB=1)

// ── FCR bits ─────────────────────────────────────────────────────────────────
#define SC16_FCR_FIFO_EN  0x01   // Enable TX and RX FIFOs (64-byte each)
#define SC16_FCR_RX_RST   0x02   // Reset RX FIFO
#define SC16_FCR_TX_RST   0x04   // Reset TX FIFO

// ── LCR values ───────────────────────────────────────────────────────────────
// 8E1: bits[1:0]=11 (8 data) | bit[2]=0 (1 stop) | bit[3]=1 (parity en)
//      bit[4]=1 (even) | bit[5]=0 (no stick) | bit[7]=0 (DLAB off)
#define SC16_LCR_8E1   0x1B
#define SC16_LCR_DLAB  0x80   // set to access divisor latches

// ── LSR bits ─────────────────────────────────────────────────────────────────
#define SC16_LSR_DR    0x01   // data ready in RX FIFO
#define SC16_LSR_TEMT  0x40   // TX holding + shift registers both empty

// =============================================================================
class SC16IS752 {
public:
    bool healthy = false;

    // Call once in setup() after Wire.begin().
    // crystalHz: on-module oscillator frequency (14745600 for 14.7456 MHz)
    // baudRate:  4800 for Gree RS485
    bool begin(TwoWire& wire, uint8_t i2cAddr, uint8_t channel,
               uint32_t crystalHz, uint32_t baudRate)
    {
        _wire = &wire;
        _addr = i2cAddr;
        _ch   = channel;   // 0 = channel A, 1 = channel B

        // Disable interrupts — we use polling
        _write(SC16_IER, 0x00);

        // Enable and reset both FIFOs
        _write(SC16_FCR, SC16_FCR_FIFO_EN | SC16_FCR_RX_RST | SC16_FCR_TX_RST);

        // Set baud rate: divisor = crystalHz / (baudRate × 16)
        uint16_t div = (uint16_t)(crystalHz / (baudRate * 16UL));
        _write(SC16_LCR, SC16_LCR_DLAB);      // open divisor latch
        _write(SC16_DLL, (uint8_t)(div & 0xFF));
        _write(SC16_DLH, (uint8_t)(div >> 8));

        // Set line format 8E1, close divisor latch
        _write(SC16_LCR, SC16_LCR_8E1);

        // Read LCR back — verifies I2C communication is working
        uint8_t lcr = _read(SC16_LCR);
        if (lcr != SC16_LCR_8E1) {
            Serial.printf("[SC16IS752] Init FAIL ch%c: LCR=0x%02X (want 0x%02X)\n",
                          _ch ? 'B' : 'A', lcr, SC16_LCR_8E1);
            healthy = false;
            return false;
        }

        Serial.printf("[SC16IS752] ch%c OK — %lu baud 8E1 (crystal=%luHz div=%u)\n",
                      _ch ? 'B' : 'A', baudRate, crystalHz, div);
        healthy = true;
        return true;
    }

    // Number of bytes waiting in RX FIFO
    uint8_t available() {
        return _read(SC16_RXLVL);
    }

    // Read one byte from RX FIFO. Returns -1 if FIFO is empty.
    int read() {
        if (!(_read(SC16_LSR) & SC16_LSR_DR)) return -1;
        return (int)_read(SC16_RHR);
    }

    // Burst-write len bytes into TX FIFO (max 64 bytes — larger than any
    // Gree response packet), then block until the shift register drains
    // (LSR[TEMT]=1). Caller manages DE pin before and after this call.
    void write(const uint8_t* buf, uint8_t len) {
        // One I2C transaction: [addr][reg][b0][b1]...[bn]
        // ESP32 Wire buffer is 128 bytes; a 50-byte packet + 1 reg = 51 bytes.
        _wire->beginTransmission(_addr);
        _wire->write(_reg(SC16_THR));
        _wire->write(buf, len);
        _wire->endTransmission();

        // Poll until TX holding register AND shift register are both empty.
        // At 4800 8E1 this takes ~115 ms for a 50-byte packet.
        while (!(_read(SC16_LSR) & SC16_LSR_TEMT));
    }

private:
    TwoWire* _wire = nullptr;
    uint8_t  _addr = 0x48;
    uint8_t  _ch   = 0;

    // I2C sub-address encoding: register[4:0] left-shifted 3, channel in bits[2:1]
    uint8_t _reg(uint8_t r) { return (r << 3) | (_ch << 1); }

    void _write(uint8_t r, uint8_t val) {
        _wire->beginTransmission(_addr);
        _wire->write(_reg(r));
        _wire->write(val);
        _wire->endTransmission();
    }

    uint8_t _read(uint8_t r) {
        _wire->beginTransmission(_addr);
        _wire->write(_reg(r));
        _wire->endTransmission(false);   // repeated start, keep bus
        _wire->requestFrom(_addr, (uint8_t)1);
        return _wire->available() ? (uint8_t)_wire->read() : 0xFF;
    }
};
