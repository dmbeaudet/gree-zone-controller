#pragma once
#include <Arduino.h>
#include "config.h"

// =============================================================================
// protocol.h — Gree UART protocol definitions
// 4800 baud, 8E1, 5V (CN port)
// Packet: [7E][7E][LENGTH][DATA...][CHECKSUM]
// Checksum = (LENGTH + sum(DATA bytes)) % 256
// Reference: bekmansurov/gree-hvac-protocol
// =============================================================================

// ── Packet framing ────────────────────────────────────────────────────────────
#define GREE_HDR1           0x7E
#define GREE_HDR2           0x7E
#define MAX_PKT_SIZE        64

// ── Outgoing (ESP32 → Air Handler) packet length bytes ───────────────────────
#define PKT_LEN_INIT        0x10   // One-time startup handshake
#define PKT_LEN_CONTROL     0x2C   // Main control packet (44 bytes payload+cksum)
#define PKT_LEN_PING        0x0E   // Keepalive ping

// ── Incoming (Air Handler → ESP32) packet length bytes ───────────────────────
#define PKT_LEN_INIT_ACK    0x03   // Handshake acknowledgement
#define PKT_LEN_STATUS_A    0x2F   // Main status response
#define PKT_LEN_STATUS_B    0x31   // Alternate status response (some firmware)

// ── Command byte (byte[7] of 0x2C control packet) ────────────────────────────
#define CMD_POLL            0x00   // Query only — no settings change
#define CMD_SET             0xAF   // Apply settings in packet

// ── Mode + fan speed byte (byte[8] of 0x2C control packet) ──────────────────
// Upper nibble = mode, lower nibble = fan speed
#define MODE_OFF            0x10
#define MODE_HEAT_AUTO      0xC0   // Heat, auto fan (unit chooses speed)
#define MODE_HEAT_LOW       0xC1
#define MODE_HEAT_MED       0xC2
#define MODE_HEAT_HIGH      0xC3
#define MODE_COOL_AUTO      0x90
#define MODE_COOL_LOW       0x91
#define MODE_COOL_MED       0x92
#define MODE_COOL_HIGH      0x93
#define MODE_AUTO_AUTO      0x80

// ── Total packet byte counts ──────────────────────────────────────────────────
// 2 header + 1 length + (length-1 data) + 1 checksum = length + 3
#define CTRL_PKT_TOTAL      (PKT_LEN_CONTROL + 3)   // = 47 bytes
#define INIT_PKT_TOTAL      (PKT_LEN_INIT    + 3)   // = 19 bytes

// ── Key byte offsets in 0x2C control packet (0-indexed from start of buf) ────
// buf[0..1] = 7E 7E header
// buf[2]    = length (0x2C)
// buf[3]    = 0x01 (fixed)
// buf[7]    = command byte
// buf[8]    = mode + fan
// buf[9]    = setpoint encoded
// buf[10]   = turbo flag
// buf[11]   = 0x02 (fixed)
// buf[12]   = swing (0x44 = off)
// buf[15]   = ECO flag
// buf[20]   = ROOM TEMP (candidate — verify during Phase 2 commissioning)
//             encoded same as setpoint: (roomTemp_C - 16) << 4
// buf[43]   = 0x02 (fixed — purpose unknown, observed in protocol RE captures)
// buf[46]   = checksum

#define PKT_OFF_CMD         7
#define PKT_OFF_MODE_FAN    8
#define PKT_OFF_SETPOINT    9
#define PKT_OFF_TURBO       10
#define PKT_OFF_SWING       12
#define PKT_OFF_ECO         15
#define PKT_OFF_ROOM_TEMP   20    // *** VERIFY during commissioning ***
#define PKT_OFF_CHECKSUM    46

// ── Key byte offsets in 0x2F/0x31 status response ────────────────────────────
// buf[4]    = power (0x04=ON, 0x00=OFF)
// buf[8]    = mode + fan (mirrors command)
// buf[9]    = setpoint (encoded)
// buf[10]   = turbo flag
// buf[15]   = ECO flag
// buf[46]   = indoor temperature: value - 40 = °C

#define STS_OFF_POWER       4
#define STS_OFF_MODE_FAN    8
#define STS_OFF_SETPOINT    9
#define STS_OFF_TURBO       10
#define STS_OFF_ECO         15
#define STS_OFF_INDOOR_TEMP 46

// =============================================================================
// Structures
// =============================================================================

struct GreePkt {
    uint8_t  raw[MAX_PKT_SIZE];
    uint8_t  totalBytes;
    uint8_t  lenByte;
    bool     valid;
};

enum class ZoneMode    : uint8_t { Off, Heat, Cool, Auto };
enum class DamperState : uint8_t { Closed, Open };
enum class SystemState : uint8_t { Off, MinOffHold, Running, MinOnHold };

// Aggregated view across all active zones — used by updateControl()
// Defined here (not in the .ino) so PlatformIO's auto-generated function
// prototypes can reference it before any function definitions are seen.
struct ZoneAggregation {
    uint8_t  activeCount   = 0;
    uint8_t  maxSetpointC  = 0;   // 0 so first active zone always overwrites
    int8_t   maxDeltaC     = 0;
    uint8_t  virtualRoomC  = DEFAULT_SETPOINT_C;
    ZoneMode dominantMode  = ZoneMode::Off;
};

struct ZoneState {
    bool        calling      = false;
    ZoneMode    mode         = ZoneMode::Off;
    uint8_t     setpointC    = DEFAULT_SETPOINT_C;
    uint8_t     roomTempC    = DEFAULT_SETPOINT_C;
    int8_t      deltaC       = 0;    // setpointC - roomTempC (positive = calling for heat)
    DamperState damper       = DamperState::Closed;
    uint32_t    lastPktMs    = 0;
    bool        commOk       = false;
    uint8_t     lastModeRaw  = MODE_OFF;  // last raw mode+fan byte from this thermostat
};

struct AHState {
    bool     powered       = false;
    uint8_t  modeRaw       = MODE_OFF;
    uint8_t  setpointC     = DEFAULT_SETPOINT_C;
    int8_t   indoorTempC   = 20;
    bool     turbo         = false;
    bool     eco           = false;
    uint32_t lastPktMs     = 0;
    bool     commOk        = false;
};

// Parser state machine
enum class ParseState : uint8_t { WaitH1, WaitH2, ReadLen, ReadData };

struct Parser {
    ParseState state  = ParseState::WaitH1;
    uint8_t    buf[MAX_PKT_SIZE] = {};
    uint8_t    idx    = 0;
    uint8_t    expect = 0;
};

// =============================================================================
// Checksum
// =============================================================================
inline uint8_t greeChecksum(const uint8_t* pkt) {
    // pkt[2] = LENGTH field
    // Sum: LENGTH + all data bytes (pkt[3] .. pkt[2+LENGTH-1])
    uint8_t  len = pkt[2];
    uint32_t sum = len;
    for (uint8_t i = 3; i < (uint8_t)(2 + len); i++) sum += pkt[i];
    return (uint8_t)(sum % 256);
}

// =============================================================================
// Packet parser — feed one byte at a time
// Returns true and populates *out when a complete packet is received.
// =============================================================================
inline bool parserFeed(Parser* p, uint8_t b, GreePkt* out) {
    switch (p->state) {
        case ParseState::WaitH1:
            if (b == GREE_HDR1) {
                memset(p->buf, 0, sizeof(p->buf));
                p->idx = 0;
                p->buf[p->idx++] = b;
                p->state = ParseState::WaitH2;
            }
            break;

        case ParseState::WaitH2:
            if (b == GREE_HDR2) {
                p->buf[p->idx++] = b;
                p->state = ParseState::ReadLen;
            } else {
                p->state = ParseState::WaitH1;
            }
            break;

        case ParseState::ReadLen:
            p->buf[p->idx++] = b;
            p->expect = b;
            if (p->expect == 0 || p->expect > (MAX_PKT_SIZE - 3)) {
                p->state = ParseState::WaitH1;
            } else {
                p->state = ParseState::ReadData;
            }
            break;

        case ParseState::ReadData:
            p->buf[p->idx++] = b;
            if (p->idx == (uint8_t)(3 + p->expect)) {
                p->state = ParseState::WaitH1;
                uint8_t rxChk  = p->buf[p->idx - 1];
                uint8_t calChk = greeChecksum(p->buf);
                out->valid      = (rxChk == calChk);
                out->lenByte    = p->buf[2];
                out->totalBytes = p->idx;
                memcpy(out->raw, p->buf, p->idx);
                return true;
            }
            break;
    }
    return false;
}

// =============================================================================
// Build 0x2C control packet (47 bytes)
// virtualRoomTempC: synthesised worst-case room temp across active zones.
//   Set to 0xFF to omit (leaves byte at 0x00 — safe for poll packets).
// =============================================================================
inline void buildControlPkt(uint8_t* out,
                             uint8_t  cmd,
                             uint8_t  modeAndFan,
                             uint8_t  setpointC,
                             uint8_t  virtualRoomTempC,
                             bool     turbo = false,
                             bool     eco   = false)
{
    memset(out, 0x00, CTRL_PKT_TOTAL);
    out[0]  = GREE_HDR1;
    out[1]  = GREE_HDR2;
    out[2]  = PKT_LEN_CONTROL;
    out[3]  = 0x01;
    out[PKT_OFF_CMD]      = cmd;
    out[PKT_OFF_MODE_FAN] = modeAndFan;
    out[PKT_OFF_SETPOINT] = TEMP_ENCODE(setpointC);
    out[PKT_OFF_TURBO]    = turbo ? 0x02 : 0x00;
    out[11]               = 0x02;
    out[PKT_OFF_SWING]    = 0x44;  // swing off
    out[PKT_OFF_ECO]      = eco   ? 0x40 : 0x00;

    // Virtual room temperature — tells outdoor inverter how hard to work.
    // Byte location TBC during Phase 2 commissioning (current best guess: byte 20).
    if (virtualRoomTempC != 0xFF) {
        out[PKT_OFF_ROOM_TEMP] = TEMP_ENCODE(virtualRoomTempC);
    }

    out[43] = 0x02;   // fixed — see byte-offset table above
    out[PKT_OFF_CHECKSUM] = greeChecksum(out);
}

// =============================================================================
// Build 0x2F status response packet (sent by ESP32 to thermostats as fake AH)
// The thermostat expects this after each poll — keeps display in sync.
// =============================================================================
inline void buildStatusResponse(uint8_t*       out,
                                 const AHState& ah,
                                 uint8_t        zoneSetpointC,
                                 int8_t         zoneRoomTempC)
{
    // 0x2F = length 0x2F = 47 data+cksum bytes → total packet = 50 bytes
    const uint8_t PKT_LEN = 0x2F;
    const uint8_t TOTAL   = PKT_LEN + 3;
    memset(out, 0x00, TOTAL);

    out[0] = GREE_HDR1;
    out[1] = GREE_HDR2;
    out[2] = PKT_LEN;
    out[3] = 0x01;
    out[STS_OFF_POWER]      = ah.powered ? 0x04 : 0x00;
    out[STS_OFF_MODE_FAN]   = ah.modeRaw;
    out[STS_OFF_SETPOINT]   = TEMP_ENCODE(zoneSetpointC);
    out[STS_OFF_TURBO]      = ah.turbo ? 0x07 : 0x00;
    out[STS_OFF_ECO]        = ah.eco   ? 0x40 : 0x00;
    // Indoor temp from zone's own room sensor (thermostat display shows this)
    out[STS_OFF_INDOOR_TEMP] = (uint8_t)(zoneRoomTempC + 40);
    out[TOTAL - 1]           = greeChecksum(out);
}

// =============================================================================
// Build one-time startup handshake packet (0x10)
// =============================================================================
inline void buildInitPkt(uint8_t* out) {
    const uint8_t known[] = {
        0x7E, 0x7E, 0x10,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x28, 0x1e, 0x19, 0x23, 0x23, 0x00,
        0xB8
    };
    memcpy(out, known, sizeof(known));
}

// =============================================================================
// Parse air handler 0x2F/0x31 status packet into AHState
// =============================================================================
inline bool parseAHStatus(const GreePkt* pkt, AHState* ah) {
    if (!pkt->valid) return false;
    if (pkt->lenByte != PKT_LEN_STATUS_A &&
        pkt->lenByte != PKT_LEN_STATUS_B) return false;

    const uint8_t* d = pkt->raw;
    ah->powered     = (d[STS_OFF_POWER]   == 0x04);
    ah->modeRaw     = d[STS_OFF_MODE_FAN];
    ah->setpointC   = TEMP_DECODE(d[STS_OFF_SETPOINT]);
    ah->turbo       = (d[STS_OFF_TURBO]   != 0x00);
    ah->eco         = (d[STS_OFF_ECO]     != 0x00);
    ah->indoorTempC = INDOOR_TEMP_DECODE(d[STS_OFF_INDOOR_TEMP]);
    ah->lastPktMs   = millis();
    ah->commOk      = true;
    return true;
}

// =============================================================================
// Debug helpers
// =============================================================================
inline void dumpPkt(const char* label, const GreePkt* pkt) {
    Serial.printf("[%s len=0x%02X valid=%s] ", label,
                  pkt->lenByte, pkt->valid ? "Y" : "N");
    for (uint8_t i = 0; i < pkt->totalBytes; i++) {
        Serial.printf("%02X ", pkt->raw[i]);
    }
    Serial.println();
}
