# Scanpay (ESP32 Relay Controller)

Small PlatformIO/Arduino project for an ESP32 that drives 4 relays, reads 4 opto inputs, and provides serial debug commands for manual testing.

## Features
- 4x relay outputs with selectable active-low or active-high polarity.
- 4x opto inputs with selectable active-low or active-high logic.
- Serial debug commands for manual relay toggling and one-shot service checks.
- WiFi setup scaffold (see `src/main.cpp`).

## Updates

### 2026-02-06
- Removed serial test command parser; status is now printed once per second.
- Added round-robin multi-channel relay scheduling on a single poll stream.
- Relay behavior: 1-second pulse, then channel is occupied until `duration_sec` cooldown expires.
- Polling for new commands only happens when at least one channel is free.

## Hardware
- ESP32 DevKit (board: `esp32dev`)
- 4-channel relay module
- 4 opto inputs (optional)

## Configuration
Edit `src/main.cpp`:
- Relay pins: `RELAY0..RELAY3`
- Opto pins: `OPTO0..OPTO3`
- Polarity: `RELAY_ACTIVE_LOW`, `OPTO_ACTIVE_LOW`
- WiFi: `WIFI_SSID`, `WIFI_PASS`

## Build & Upload (PlatformIO)
```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## Serial Commands (115200 baud)
```
help
state <ch> <durationMs>
svc <ch> <on|off> <durationMs> <opto on|off>
```

### Examples
```
state 0 5000
svc 1 on 3000 off
```

Notes:
- `state` toggles the relay state for a channel and records `durationMs`.
- `svc` enables a periodic service handler (checked once per second) that performs the timed 1s pulse/off behavior.

## File Layout
- `src/main.cpp` - main application logic
- `platformio.ini` - PlatformIO environment config
- `include/` - headers (if needed)


# Development Log for the Project

## 🎯 Objectives

- [X] Design and implement a QR-code based payment workflow (mock → real-ready)
- [ ] Achieve ≥95% functional accuracy for payment-to-locker activation
- [ ] Multiple Channel unlock and under single QR Code
- [ ] API-Based Communication between server and client device (ESP32)
- [ ] Integrate ESP32-controlled relay to actuate locker mechanism
- [X] Optimize system for low power consumption (≥20% reduction vs baseline)
- [X] Ensure system reliability under intermittent Wi-Fi conditions
- [X] Provide basic status feedback (LED / log-based)


## 📦 Deliverables

### Software
- [X] Backend service for QR payment session handling
- [X] Mock QR payment verification workflow
- [X] API endpoints for payment confirmation & device trigger
- [X] Transaction logging using SQLite database
- [ ] Error handling and retry logic for failed or delayed payments

### Firmware
- [X] ESP32 firmware for Wi-Fi connectivity
- [ ] Secure communication with backend API (HTTP/TLS)
- [X] GPIO control logic for relay activation
- [X] Timed pulse control to prevent solenoid overheating
- [ ] Fallback handling for network instability

### Hardware
- [X] ESP32-based controller board
- [X] Relay driver circuit for 12V solenoid
- [ ] Solenoid-based locker locking mechanism
- [ ] Basic enclosure / mechanical mounting
- [ ] Optional LED status indicator

### Documentation
- [x] System architecture overview
- [x] Development progress log (PROGRESS.md)
- [ ] Hardware wiring diagrams
- [ ] Firmware build & flash instructions
- [ ] Final integration and test report


## ✅ Development Checklist

### Architecture & Planning
- [x] System architecture defined
- [x] Hardware platform selected
- [x] Locking mechanism selected

### Software
- [X] QR code generation
- [X] Payment session handling
- [X] Backend verification logic
- [X] Database logging
- [ ] API security hardening

### Firmware
- [X] ESP32 Wi-Fi connection
- [X] API request handling
- [ ] Relay actuation logic
- [ ] Pulse-timed solenoid control
- [ ] Retry & timeout handling
- [ ] Multi Channel Control

### Integration & Testing
- [ ] End-to-end payment → unlock test
- [ ] Latency measurement
- [ ] Power consumption measurement
- [ ] Failure mode testing (network / power loss)


# System Architecture
Will be updated within 3 Feb 2025

# System BOM and SLD Diagram 
Will be updated within 3 Feb 2025

## ESP32 Development Progress (QR Smart Locker)

### 2026-01-22
- Defined ESP32 as the main service activation controller
- Confirmed architecture: Backend → ESP32 → Relay → Solenoid Lock
- Identified ESP32 responsibilities (Wi-Fi, API handling, GPIO actuation)

### 2026-01-24
- Finalized ESP32 hardware selection (ESP32-WROOM-32 Relay Module)
- Defined GPIO usage for relay control and optional LED status from the module design
- Reviewed power domain separation (3.3V logic / 12V actuation)

### 2026-01-26
- Defined relay-driven solenoid control strategy
- Identified solenoid overheating risk under continuous activation
- Decided on pulse-timed actuation logic (software-controlled)

### 2026-01-27
- Planned ESP32 firmware flow:
  - Wi-Fi connect
  - Backend polling / callback handling
  - Conditional GPIO trigger
- Defined retry and timeout behavior for unstable network conditions

### 2026-01-29
- Integrated ESP32 role with mock QR payment backend ( Backend System )
- Defined unlock trigger conditions based on payment verification
- Documented ESP32 integration flow for client-facing progress report

### 2026-01-30
- Prepared GitHub-based workflow:
  - README.md for system overview
  - PROGRESS.md as live development log
- Mapped ESP32 tasks to project objectives and deliverables

### Next
- Implement ESP32 firmware (Wi-Fi + API handling)
- Test relay + solenoid actuation with timed pulse
- Perform end-to-end payment → unlock validation
- Hardware design and Installation
- Multi Locker control within same backend architecture


