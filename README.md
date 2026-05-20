# Gree Zone Controller

Custom 3-zone HVAC controller for the **Gree FXE48HP230V1R32AH** ducted air handler.

Replaces a conventional 24V zone controller (Emerson EMM-3) while retaining
full Gree RS485 serial communication — restoring variable-speed blower control
and energy tracking that are lost in standard 24V zone-controller setups.

## The Problem

The Gree FXE ducted air handler uses a proprietary RS485 serial protocol
(H1/H2) for full functionality. Conventional zone controllers require 24V
signalling, which forces the unit into a degraded mode — losing variable fan
speed, energy monitoring, and advanced heat pump features.

No commercial product solves this in the North American market.

## The Solution

An ESP32-S3 acts as a protocol bridge:

```
[WK-010WC1 Stat 1] --RS485--> |                 | --UART/5V--> [Air Handler CN port]
[WK-010WC1 Stat 2] --RS485--> |  ESP32-S3       | --Relay 1--> [Zone 1 Damper]
[WK-010WC1 Stat 3] --RS485--> |  Zone Controller| --Relay 2--> [Zone 2 Damper]
                               |                 | --Relay 3--> [Zone 3 Damper]
                               |                 | --WiFi-----> [Home Assistant]
```

- Reads each thermostat's demand over RS485
- Aggregates 3 zones into a single command to the air handler
- Sets blower speed proportionally (1 zone=low, 2=med, 3=high)
- Controls zone dampers via relay outputs
- Reports all state to Home Assistant via MQTT

## Hardware

| Component | Purpose |
|-----------|---------|
| ESP32-S3 DevKit | Main controller |
| 3× MAX485 module | Thermostat RS485 buses |
| Voltage divider (10k/20k) | 5V→3.3V level shift on CN port RX |
| 3-channel relay board | Damper control (24VAC) |
| 24VAC 40VA transformer | Power for thermostats + relays |
| 24VAC→5V buck converter | Powers ESP32 |

## Firmware

Written for **Arduino IDE** / **ESP-IDF** targeting ESP32-S3.

**Dependencies** (install via Arduino Library Manager):
- `PubSubClient` by Nick O'Leary
- `EspSoftwareSerial` by Dirk Kaar

**Setup:**
```bash
cp firmware/GreeZoneController/secrets.h.template \
   firmware/GreeZoneController/secrets.h
# Edit secrets.h with your WiFi and MQTT details
```

Open `firmware/GreeZoneController/GreeZoneController.ino` in Arduino IDE,
select **ESP32S3 Dev Module**, and flash.

## Commissioning

See [docs/COMMISSIONING.md](docs/COMMISSIONING.md) for step-by-step
wiring verification and phase-by-phase startup procedure.

## Protocol Reference

Based on reverse-engineering documented at:
[bekmansurov/gree-hvac-protocol](https://github.com/bekmansurov/gree-hvac-protocol)

**UART params:** 4800 baud, 8E1, 5V  
**Packet format:** `[7E][7E][LENGTH][DATA...][CHECKSUM]`  
**Checksum:** `(LENGTH + sum(DATA)) % 256`

## Status

| Feature | Status |
|---------|--------|
| CN port packet parser | ✅ v0.1 |
| 0x2C control packet builder | ✅ v0.1 |
| 0x2F status parser | ✅ v0.1 |
| Phase 1/2/3 boot sequence | ✅ v0.1 |
| Zone aggregation + damper control | ✅ v0.1 |
| MQTT / Home Assistant reporting | ✅ v0.1 |
| Thermostat RS485 bridge (slave mode) | 🔧 v0.2 |
| Thermostat 0x2F response packets | 🔧 v0.2 |
| Anti-short-cycle protection | 🔧 v0.2 |
| OTA firmware updates | 🔧 v0.3 |

## License

MIT
