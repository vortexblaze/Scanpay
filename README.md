# Scanpay (ESP32 Relay Controller)

Small PlatformIO/Arduino project for an ESP32 that drives 4 relays, reads 4 opto inputs, and provides serial debug commands for manual testing.

## Features
- 4x relay outputs with selectable active-low or active-high polarity.
- 4x opto inputs with selectable active-low or active-high logic.
- Serial debug commands for manual relay toggling and one-shot service checks.
- WiFi setup scaffold (see `src/main.cpp`).

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

## License
Add your preferred license here.
