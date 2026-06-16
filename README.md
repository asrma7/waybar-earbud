# Waybar Earbud Battery

Small Linux Waybar helper for earbud battery status.

The default mode follows the current Bluetooth audio output, auto-detects the
device type, and prints Waybar JSON with left, right, and case battery when
available.

Supported providers:

- Sony MDR over Bluetooth Classic RFCOMM
- Apple AirPods over BlueZ Profile1 / Apple Accessory Protocol

Provider code lives under `src/devices/` so future device support can be added
without touching unrelated protocol code.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Requires BlueZ and GLib/GIO development headers. Package names are commonly
`bluez-libs-devel`, `libbluetooth-dev`, `glib2-devel`, `libglib2.0-dev`, or
similar. Default-output detection uses `pactl`, which works with PulseAudio and
PipeWire's PulseAudio compatibility service.

## Install

From a release:

```sh
curl -fsSL https://raw.githubusercontent.com/asrma7/waybar-earbud/main/install.sh | bash
```

The installer downloads the matching `x86_64` or `aarch64` Linux release asset and
installs `waybar-earbud` to `~/.local/bin` by default. Override the
destination with `PREFIX=/usr/local ./install.sh` or
`BINDIR=/some/bin ./install.sh`.

Uninstall from the default location:

```sh
curl -fsSL https://raw.githubusercontent.com/asrma7/waybar-earbud/main/install.sh | bash -s -- --uninstall
```

For custom install locations, pass the same destination option used during
install:

```sh
PREFIX=/usr/local ./install.sh --uninstall
BINDIR=/some/bin ./install.sh --uninstall
```

## Test

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite avoids real Bluetooth hardware. It covers default audio sink MAC
parsing, shared Waybar JSON output, and the no-device CLI fallback.

## Run

Follow the current Bluetooth audio output:

```sh
./build/waybar-earbud
```

Pass a MAC address explicitly for testing:

```sh
./build/waybar-earbud --mac AA:BB:CC:DD:EE:FF
```

Force a provider for debugging:

```sh
./build/waybar-earbud --device sony
./build/waybar-earbud --device airpods
./build/waybar-earbud --device sony --mac AA:BB:CC:DD:EE:FF
./build/waybar-earbud --device airpods --mac AA:BB:CC:DD:EE:FF
```

By default the command prints one JSON object and exits. Use `--watch` to keep
the process alive. In watch mode, `SIGUSR1` toggles the Bluetooth connection:

```sh
./build/waybar-earbud --watch --mac AA:BB:CC:DD:EE:FF
```

For Sony, `--watch` refreshes every 30 seconds unless an interval is provided:

```sh
./build/waybar-earbud --watch --device sony --mac AA:BB:CC:DD:EE:FF --interval 10
```

For Sony, `SIGUSR1` calls BlueZ `Disconnect`/`Connect` and pauses polling while
manually disconnected. For AirPods, `--watch` keeps the BlueZ Profile1/AAP
monitor alive and uses `SIGUSR1` as the connect/disconnect toggle.

## Waybar

Sony one-shot module:

```json
"custom/earbuds": {
  "exec": "/path/to/waybar-earbud",
  "return-type": "json",
  "interval": 30
}
```

When no supported Bluetooth audio output is active, the helper emits empty text
with `class: "disconnected"`:

```css
#custom-earbuds.disconnected {
  margin: 0;
  padding: 0;
}
```

AirPods can also be used as a persistent module:

```json
"custom/earbuds": {
  "exec": "/path/to/waybar-earbud --watch --mac AA:BB:CC:DD:EE:FF",
  "return-type": "json",
  "signal": 1,
  "on-click": "pkill -SIGUSR1 waybar-earbud"
}
```

Manual toggle:

```sh
pkill -SIGUSR1 waybar-earbud
```

## Adding Providers

Add a folder under `src/devices/<name>/` with:

- a cheap `available(mac)` detector
- a reader/monitor that emits the shared Waybar JSON shape
- a route in `src/main.cpp`
- watch-mode behavior that handles `SIGUSR1` as a connect/disconnect toggle

Provider watch mode should keep the process alive, emit JSON whenever state
changes, and use `SIGUSR1` consistently with Waybar's `signal` setting. When
the signal disconnects the device, print the shared disconnected JSON shape and
pause provider polling. When the next signal reconnects it, resume provider
polling/monitoring and emit fresh battery JSON once available.

Expected JSON shape:

```json
{"text":"81%","tooltip":"Device\nL: 82%\nR: 80%\nCase: 90%","class":"good","alt":"connected"}
```

## Credits

This project builds on protocol work and implementation ideas from:

- [mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient)
- [Silverquark/waybar-airpods-module](https://github.com/Silverquark/waybar-airpods-module)
