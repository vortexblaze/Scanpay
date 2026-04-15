# Scanpay (ESP32 Relay Controller)

PlatformIO/Arduino firmware for an ESP32 that polls a backend for device commands, drives two fixed relay channels, and requests invoice generation after a relay task finishes.

## Current Firmware Behavior
- Uses `Relay0` and `Relay1` only.
- Maps devices to fixed channels:
  - `DEV001 -> Relay0`
  - `DEV002 -> Relay1`
- Uses non-blocking relay timing based on `millis()`.
- Polls the backend in a background FreeRTOS task.
- Queues pending relay commands and invoice requests in firmware.
- Generates invoices when a relay task completes successfully.
- Uses WiFiManager for runtime Wi-Fi and parameter configuration.
- Prints one compact UART status line instead of verbose debug logs.

## Firmware Changelog
### 2026-04-05
- Updated ESP32 invoice generation to use the backend contract at `POST /api/device/<device_id>/request-invoice/`.
- Changed invoice request payload to:
  - `amount` as a string
  - `description` as `ESP32 auto invoice`
  - `duration_sec` from firmware config
- Added full invoice response reading before closing the HTTP connection to reduce backend `Broken pipe` errors.
- Added JSON parsing for invoice response fields:
  - `public_id`
  - `pay_url`
- Added invoice HTTP timeout tuning and disabled HTTP reuse for the invoice request path.
- Updated invoice processing so it only sends when the mapped relay channel for that device is available.
- Changed loop order so invoice requests are processed before pending relay commands reclaim the channel.
- Added UART print of the invoice API URL for easier live debugging on the serial monitor.

### 2026-04-01
- Added parallel relay switching support for the two active relay channels.
- Changed device handling from shared relay selection to fixed mapping:
  - `DEV001 -> Relay0`
  - `DEV002 -> Relay1`
- Enabled dual device polling in the ESP32 worker task so both device IDs can be checked in the same polling cycle.
- Reworked command handling into a pending command queue so two relay tasks can be staged without clobbering one another.
- Reworked invoice generation into a queued flow so completed relay tasks from both device IDs can independently trigger invoice requests.
- Moved invoice triggering to actual relay-task completion instead of the older per-device timer state.
- Added relay watchdog cleanup so a stuck relay task is force-released without generating a false invoice.
- Replaced verbose UART helper/debug logs with a compact once-per-second status indicator.

## Pending Tasks
- Verify end-to-end backend compatibility for:
  - `GET /api/device/<device_id>/next/`
  - `POST /api/device/<device_id>/request-invoice/`
- Build and flash test the current firmware on hardware after the recent queueing and fixed-mapping refactor.
- Validate dual-device parallel switching on real hardware:
  - `DEV001` should trigger `Relay0`
  - `DEV002` should trigger `Relay1`
- Confirm invoice generation behavior for both device IDs under back-to-back and simultaneous completions.
- Decide whether failed invoice POSTs should retry with backoff or remain queued indefinitely.
- Consider moving invoice HTTP POST handling off the main loop if blocking becomes visible during field tests.
- Split `src/main.cpp` into smaller modules:
  - Wi-Fi/config
  - polling/state management
  - relay state machine
  - invoice queue/HTTP handling
- Remove the fixed-time invoice flow and move toward login-based user-triggered invoice generation.
- Design and implement UI handling for login-based user-trigger flow with the ESP32 backend/device integration.
- Add a brief hardware wiring section and real UART status examples to the documentation.

## Relay Flow
1. ESP32 polls `/api/device/<device_id>/next/`.
2. If a command is returned, it is accepted only for that device's mapped relay.
3. The command is queued until its mapped relay is free.
4. The relay is driven with a short start pulse, held for `duration_sec`, then driven with a short stop pulse.
5. When the relay task finishes normally, the firmware queues `/api/device/<device_id>/request-invoice/`.
6. Invoice POST is only sent when that device's mapped relay channel is available.

## Backend Contract Used by Firmware
### Poll Endpoint
`GET /api/device/<device_id>/next/`

Expected response patterns:

```json
{"has_command": false}
```

```json
{"has_command": true, "action": 1, "duration_sec": 5, "command_id": 123}
```

### Invoice Endpoint
`POST /api/device/<device_id>/request-invoice/`

Request body:

```json
{"amount": "5.00", "description": "ESP32 auto invoice", "duration_sec": 240}
```

Response fields used by firmware:
- `public_id`
- `pay_url`

## Hardware
- ESP32 DevKit (`esp32dev`)
- Relay outputs:
  - `RELAY0 = GPIO23`
  - `RELAY1 = GPIO5`
- Reserved but currently unused relay pin definitions:
  - `RELAY2 = GPIO4`
  - `RELAY3 = GPIO13`
- Opto inputs:
  - `OPTO0 = GPIO25`
  - `OPTO1 = GPIO26`
  - `OPTO2 = GPIO27`
  - `OPTO3 = GPIO33`
- Wi-Fi config trigger:
  - `WIFI_CONFIG_PIN = GPIO17`

## Configuration
Runtime parameters are stored in Preferences and exposed through WiFiManager:
- `host_ip`
- `device_id`
- `device_id_2`
- `price`
- `inv_duration`
- `description`

Firmware constants in `src/main.cpp`:
- `DEVICE2_ENABLED`
- `HTTP_POLL_INTERVAL_MS`
- `HTTP_TIMEOUT_MS`
- `INVOICE_HTTP_TIMEOUT_MS`
- `RELAY_PULSE_MS`
- `RELAY_WATCHDOG_GRACE_MS`
- `STATUS_INTERVAL_MS`
- `RELAY_ACTIVE_LOW`
- `OPTO_ACTIVE_LOW`

## UART Status Indicator
Baud rate: `115200`

The firmware now prints one compact status line:

```text
S W1 C0 R10 Q0 T10 I0 O0000
```

Meaning:
- `W`: Wi-Fi connected (`1` or `0`)
- `C`: WiFiManager config pin active
- `R`: relay states for `Relay0` and `Relay1`
- `Q`: pending command count
- `T`: active task count for `DEV001` and `DEV002`
- `I`: pending invoice queue count
- `O`: opto input states `OPTO0..OPTO3`

## Build & Upload (PlatformIO)
```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## File Layout
- `src/main.cpp` - firmware logic
- `platformio.ini` - PlatformIO environment config
- `include/` - optional headers


# Development Log for the Project

## 🎯 Objectives

- [X] Design and implement a QR-code based payment workflow (mock → real-ready)
- [ ] Achieve ≥95% functional accuracy for payment-to-locker activation
- [ ] Multiple Channel unlock and under single QR Code ( 2 Units )
- [ ] API-Based Communication between server and client device (ESP32)
- [ ] Integrate ESP32-controlled relay to actuate locker mechanism
- [ ] Optimize system for low power consumption (≥20% reduction vs baseline) ( Require Proof )
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
- [ ] Multi Channel Lock Control 

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
 ## This Include 
 - Component Selection Reason
 - Wiring & Signal Flow
 - BOM


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

### 2026-04-01
- Refactored the ESP32 firmware from a simple relay trigger flow into a queued two-device controller.
- Limited active relay control to two channels and fixed the mapping:
  - `DEV001 -> Relay0`
  - `DEV002 -> Relay1`
- Reworked relay timing into a non-blocking state machine using `millis()`.
- Moved backend polling into a FreeRTOS background task to avoid blocking the main loop.
- Added pending command queueing so relay jobs can be staged and dispatched independently.
- Added relay watchdog handling to force-release stuck relay tasks.
- Moved invoice generation to a completion-based queue so each successful relay task can request its own invoice.
- Simplified UART output to a compact periodic status indicator.
- Updated `README.md` to reflect the current firmware architecture, backend contract, changelog, and pending tasks.

### 2026-04-05
- Aligned the ESP32 invoice request flow with the backend requirement for `POST /api/device/<device_id>/request-invoice/`.
- Updated invoice request body to send `amount` as a string and use `ESP32 auto invoice` as the description.
- Added full response-body read and JSON parsing for `public_id` and `pay_url` before closing the HTTP connection.
- Added invoice API URL printing on UART to support live endpoint verification during field debugging.
- Updated invoice processing to wait until the mapped relay channel is available before sending the invoice request.
- Reordered the main loop so invoice processing runs before pending relay commands reclaim a freed channel.
- Recorded the next flow for:
  - removal of fixed-time invoice generation toward login-based user trigger
  - UI handling for login-based user trigger with ESP32 integration

### Next
- Implement ESP32 firmware (Wi-Fi + API handling)
- Test relay + solenoid actuation with timed pulse
- Perform end-to-end payment → unlock validation
- Hardware design and Installation
- Multi Locker control within same backend architecture

