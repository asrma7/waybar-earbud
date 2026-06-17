# Waybar Earbud Battery

Small Linux Waybar helper for earbud battery status.

`waybar-earbud` runs as one foreground service that owns Bluetooth provider
state, then Waybar runs lightweight clients that read Waybar JSON from the
service over a Unix socket.

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

The installer downloads the matching `x86_64` or `aarch64` Linux release asset
and installs `waybar-earbud` to `~/.local/bin` by default. Override the
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
parsing, shared Waybar JSON output, and CLI behavior.

## Run

Start one service. It follows the current Bluetooth audio output by default and
auto-detects the provider:

```sh
waybar-earbud --service
```

Run clients from Waybar or a shell:

```sh
waybar-earbud          # print one JSON object from the service
waybar-earbud --watch  # stream JSON updates from the service
waybar-earbud --toggle # toggle service connect/disconnect
```

The service socket is `$XDG_RUNTIME_DIR/waybar-earbud.sock`, falling back to a
user-scoped path under `/tmp` when `XDG_RUNTIME_DIR` is unavailable.

Service options:

```sh
waybar-earbud --service --device sony
waybar-earbud --service --device airpods
waybar-earbud --service --mac AA:BB:CC:DD:EE:FF
waybar-earbud --service --device sony --mac AA:BB:CC:DD:EE:FF --interval 10
```

`--mac` pins the service to one device. Without `--mac`, the service monitors
the current Bluetooth audio output and re-execs itself when the output changes.

Sony battery reads refresh every 30 seconds by default, with a minimum interval
of 5 seconds. Sony connection and disconnection are event-driven through BlueZ;
battery read failures after reconnect retry briefly before returning to the slow
refresh interval.

If the service is not running, clients emit disconnected JSON and exit in
one-shot mode or keep retrying in `--watch` mode.

Waybar custom modules only consume a fixed set of JSON fields such as `text`,
`tooltip`, `class`, and `alt`; arbitrary fields like `left` and `case` are not
available in Waybar's `format` string. Use a preset or client text templates to
choose what goes into Waybar's `text`.

Presets:

```sh
waybar-earbud --watch --preset battery   # 82%, blank when disconnected
waybar-earbud --watch --preset split     # L:82% R:80% C:90%, blank when disconnected
waybar-earbud --watch --preset icon      # ◀ 82% ▶ 80% ▣:90%, blank when disconnected
```

The tooltip is always the device name plus status, such as
`WF-1000XM6 Connected`. The `alt` field is always only the status:
`connected`, `connecting`, or `disconnected`.

Advanced templates:

```sh
waybar-earbud --watch --connected-format '{battery}%' --disconnected-format ''
waybar-earbud --watch --connected-format 'L:{left}% R:{right}%{?case} C:{case}%{/case}' --disconnected-format ''
waybar-earbud --watch --format '{status}'
```

Available template fields are `{text}`, `{battery}`, `{left}`, `{right}`,
`{case}`, `{status}`, `{class}`, `{alt}`, and `{device}`. Missing charge values
render as an empty string. Use status-specific templates such as
`--connected-format`, `--connecting-format`, and `--disconnected-format` when
punctuation or suffixes should only appear for one state.

Optional blocks render only when a field exists:

```sh
waybar-earbud --watch --connected-format 'L:{left}% R:{right}%{?case} C:{case}%{/case}'
```

## Toggle

Use the client toggle command for Waybar clicks. It sends a control request to
the service over the same Unix socket used for JSON updates.

```sh
waybar-earbud --toggle
```

## Waybar

Run the service once from your compositor, shell startup, or a systemd user
unit:

```sh
waybar-earbud --service
```

Waybar module:

```json
"custom/earbuds": {
  "exec": "waybar-earbud --watch --preset battery",
  "return-type": "json",
  "on-click": "waybar-earbud --toggle"
}
```

Do not set Waybar's `signal` option for this module. `signal` reloads the
Waybar custom client process; service actions should be sent through
`waybar-earbud --toggle`.

When no supported Bluetooth audio output is active, the helper emits empty
`text` and `class: "disconnected"`:

```css
#custom-earbuds.disconnected {
  margin: 0;
  padding: 0;
}
```

## JSON

Default disconnected client JSON:

```json
{"text":"","class":"disconnected","alt":"disconnected","tooltip":"Earbuds Disconnected"}
```

Default connected client JSON:

```json
{"text":"82%","class":"good","alt":"connected","tooltip":"WF-1000XM6 Connected"}
```

Use `--format` to render structured service fields into Waybar-supported
`text`. The tooltip is fixed to device name plus status. The default
`{battery}` value uses the best available earbud value rather than averaging
left and right, because some Sony devices report `0%` for a bud that is in the
case.

## Adding Providers

Add a folder under `src/devices/<name>/` with:

- a cheap `available(mac)` detector
- a reader or monitor that emits the shared Waybar JSON shape
- a route in `src/main.cpp`
- service-mode behavior compatible with `--toggle` as a connect/disconnect toggle

Provider code should use the shared `Battery` and JSON helpers in
`src/common.hpp`. Unpinned default-output rediscovery is automatic in the
service.

## Credits

This project builds on protocol work and implementation ideas from:

- [mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient)
- [Silverquark/waybar-airpods-module](https://github.com/Silverquark/waybar-airpods-module)
