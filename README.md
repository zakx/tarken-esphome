# Tarken WM ESPHome bridge

ESP32 firmware (ESPHome external component) that connects over BLE to a **Tarken WM**
window opener. It sends window commands and polls the sensor data, and shows both in
Home Assistant as one device. Protocol: [`TARKEN_BLE_API.md`](TARKEN_BLE_API.md).

Tested with Tarken WM firmware 1.1.4, an ESP32 (ESP32-D0WD-V3) and ESPHome 2026.10.
ESPHome 2026.10 or newer is required (OTA encryption with the API key, digest auth).

## Entities

| Entity | Function |
|---|---|
| Close | `{"window":0}` |
| Micro tilt | `{"window":7}` then `{"window":1}` |
| Tilt fully | `{"window":8}` then `{"window":1}` |
| Open fully | `{"window":2}` |
| Refresh sensors | poll now |
| Poll interval (config) | seconds between polls, 0 = off, stored in flash |
| Pair (config) | get the pair key (see [Pairing](#pairing)) |
| Forget Tarken (config) | forget the stored MAC and search again |
| Temperature, Humidity, CO2, TVOC, Battery | sensor data |
| Window state | commanded state: Closed / Open |
| Tarken firmware, Tarken uptime, Tarken MAC, BLE connected, Last message | diagnostics |
| Raw command (diagnostic) | send any JSON to the Tarken |

## How it works

- The bridge connects **only for a session** (a poll or a command), then disconnects.
  This saves the Tarken battery and keeps the phone app usable.
- Every session **logs in** first (challenge/response with the pair key). After the
  login the Tarken sends its full status. A poll ends after this status, in about 3.5 s.
- Commands are written with response, one after the other. The next one is sent only
  after the write response, and a failed write cancels the rest of the sequence.
- After a command the bridge disconnects. Then it polls every `status_interval` (10 s)
  until the window reports the target state, for at most `command_hold_time` (120 s).
  Movements take about one minute. The Tarken reports the commanded state, not the
  position (see the protocol document).
- The Tarken is found by its advertised name (`Tarken WM`). Its MAC address is stored in flash.

## Pairing

The bridge needs the Tarken's pair key. The Tarken sends it only in the first ~60 s
after it is powered on:

1. Flash the bridge and add it to Home Assistant.
2. Power-cycle the Tarken (remove and insert the battery, or similar).
3. Within 60 s, press **Pair**. The key is stored on the bridge (log: "Pair key received and stored").

You can also put the key in `secrets.yaml` (`tarken_pair_uid`). A key received with
**Pair** replaces it and is kept over reboots.

## Install

### ESPHome Device Builder (Home Assistant app)

Use the component straight from this repository:

```yaml
external_components:
  - source: github://zakx/tarken-esphome
    components: [tarken]
```

Copy the rest of [`tarken.yaml`](tarken.yaml) (without its `external_components`
block), and add the secrets from [`secrets.example.yaml`](secrets.example.yaml).

### Local build

```sh
python3.14 -m venv .venv && .venv/bin/pip install esphome   # Python 3.12–3.14
cp secrets.example.yaml secrets.yaml      # fill in
.venv/bin/esphome run tarken.yaml --device /dev/ttyUSB0   # first flash over USB
./ota.sh <bridge-ip>       # later updates over WiFi (web server upload)
./logs.sh <bridge-ip>      # live log over WiFi
```

The web page (`http://<bridge-ip>/`), the web OTA upload and the logs use HTTP digest
auth with the user `admin` and the `ota_password` from `secrets.yaml`. The esphome OTA
(esphome tool, Device Builder) is encrypted with the API key.

On macOS, `esphome upload/logs --device <ip>` can fail with "No route to host" until
the terminal app is allowed in System Settings → Privacy & Security → Local Network.
`ota.sh` and `logs.sh` use `curl`, which does not have this problem.

## Options (`tarken:`)

| Option | Default | Meaning |
|---|---|---|
| `ble_client_id` | — | the `ble_client` to use (`auto_connect: false`) |
| `poll_interval_number` | — | `number` entity for the poll interval |
| `poll_interval` | 300 s | used if no number entity is given |
| `pair_uid` | — | pair key, `"uid0 uid1 uid2 uid3"` (hex) |
| `mac_address` | auto | fixed MAC; skips the search |
| `idle_timeout` | 10 s | end a session after this time with no traffic |
| `session_timeout` | 60 s | give up when the Tarken is not found / does not connect |
| `max_connected_time` | 30 s | max link time for a poll |
| `command_delay` | 500 ms | pause between two commands (e.g. `8` then `1`) |
| `status_interval` | 10 s | poll interval while a movement runs |
| `command_hold_time` | 120 s | max time to wait for the target state |

`stage1/scan.yaml` is a small firmware that only logs BLE advertisements (USB log),
to find the Tarken.

## License

See [LICENSE](LICENSE).
