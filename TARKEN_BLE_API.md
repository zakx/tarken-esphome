# Tarken WM BLE API

Unofficial description of the BLE protocol of the Tarken WM window opener, as used by the
ESPHome component in [`components/tarken/`](components/tarken/). Marked **[tested]** means
tested on a device with firmware 1.1.4. The rest is not tested.

---

## 1. Device [tested]

| Item | Value |
|---|---|
| Advertised name | `Tarken WM` |
| Address type | static random |
| Advertisement | no service UUID list; Matter commissioning service data (`0xFFF6`) |
| GATT service | `0000abf0-0000-1000-8000-00805f9b34fb` (`0xABF0`) |
| Characteristic | `0000abf1-0000-1000-8000-00805f9b34fb` (`0xABF1`) |
| Properties | write-without-response, write, notify |
| MTU | 247 negotiated without problems |
| BLE pairing/bonding | not used |

The advertisement does not contain `0xABF0`, so find the device by its name.

---

## 2. Connection sequence [tested]

1. Connect.
2. Discover the services; find `0xABF0` and characteristic `0xABF1`.
3. Enable notifications (write `0x0001` to the CCCD).
4. Log in (section 6): `{"challenge":1}` → 4 nonces → 4 responses → `{"ack":"auth_ok"}`.
5. After `auth_ok` the device sends its full status (section 5).
6. Send commands, written **with response**, one at a time (send the next only after the
   write response of the previous one).
7. Disconnect.

Without a login, `{"ping":1}` works, but `{"window":N}` gets a write response and is
then ignored, no status is sent, and the device drops the link after about 90 s.
A logged-in idle link is also dropped after about 90 s.

---

## 3. Wire format [tested]

Commands and notifications are **plain UTF-8 JSON** objects, one object per write or
notification. Example: the 12 bytes `{"window":0}`. Base64-encoded commands are not answered.

Parse each notification on its own and update only the fields it contains. All messages
seen fit in one notification at MTU 247 (largest: 76 bytes).

Some firmware builds may send an unformatted printf token such as `%1.1f` in place of a
number. Replace `%\.?\d*f` with a placeholder (e.g. `-69`) before you parse, and treat that
value as missing.

---

## 4. Commands (`{"window": N}`)

| N | Meaning | |
|---:|---|---|
| `0` | Close | [tested] |
| `1` | Open in the selected mode (see `7` / `8`) | [tested] |
| `2` | Open fully | [tested] |
| `3`–`6` | Manual jog (four directions; repeat every 300 ms while held) | |
| `7` | Select micro-tilt mode (no movement by itself) | [tested] |
| `8` | Select full-tilt mode (no movement by itself) | [tested] |
| `9` | Cancel automatic mode | |
| `10` / `11` | Disable / enable the physical buttons | |
| `12` / `13` | Disable / enable the spindle | |

### Recipes [tested]

```text
{"window":0}                    close
{"window":7} then {"window":1}  micro tilt
{"window":8} then {"window":1}  full tilt
{"window":2}                    open fully
```

Send `1` only after the write response for `7` / `8`. A movement takes about one minute.

### Other commands

| JSON | Purpose | |
|---|---|---|
| `{"ping":1}` | keepalive → `{"ack":"ping"}`, works without login | [tested] |
| `{"pair":1}` | get the pair key (section 6) | [tested] |
| `{"challenge":1}` | start the login (section 6) | [tested] |
| `{"mode":"timer","status":0/1,"time":N}` | timer | |

There is **no "get status" command**. The status is sent only after a login on a
**new** connection. A second `{"challenge":1}` on a connection that is already logged in gives
`auth_ok` again, but no new status [tested]. To read the state again, reconnect and log in.

---

## 5. Status [tested]

After `auth_ok` the device sends, one notification each:

```json
{"temp":19.0}
{"hum":74.00}
{"co2":1020}
{"tvoc":0}
{"window":1}
{"fw":"1.1.4", "open":2, "active":2, "up":1409885, "spindle":1, "buttons":1}
{"bat":100}
{"auto":[-1, 0, 0.00, 0, 0, 0, 0.00]}
{"sched_en":0}
```

While the link stays open, `{"sched_en":0}` (sometimes also `auto`) is sent again every few
seconds. The first `sched_en` marks the end of the status, so a poll can disconnect there.

| Key | Type | Meaning |
|---|---|---|
| `temp` | float | temperature, °C [tested] |
| `hum` | float | relative humidity, % [tested] |
| `co2` | int | CO2, ppm [tested] |
| `tvoc` | int | TVOC [tested, 0 so far] |
| `bat` | int | battery, % [tested] |
| `window` | int | **commanded** state: `0` closed, `1` open [tested] |
| `open` | int | selected mode: `1` full tilt (after `8`), `2` micro tilt (after `7`) [tested] |
| `active` | int | `1` / `2` seen; meaning unknown (not "motor running") |
| `up` | uint | device uptime, s |
| `spindle` | int | spindle enabled (seen `1`) |
| `buttons` | int | physical buttons enabled (seen `1`) |
| `fw` | string | firmware version |
| `auto` | array (7) | automation settings; slots 3 and 7 are floats |
| `timer` | array | timer state |
| `sched_en` | int | schedule enabled |
| `ack` | string | `ping`, `auth_ok`, `auth_fail`, `pair_closed` |
| `nonce0`–`nonce3` | hex string | login challenge (section 6) |
| `uid0`–`uid3` | hex string | pair key (section 6) |

**`window` is not the position.** It changes to the commanded state at once, and the
motor needs about a minute after that. `{"window":2}` (open fully) reports `window: 1`.
No field shows that a movement has finished.

---

## 6. Pairing and login [tested]

All values in this section are examples. The login example uses the example key, so
you can use it to test an implementation.

### Pairing (once)

```json
→ {"pair":1}
← {"uid0":"00112233"}
← {"uid1":"44556677"}
← {"uid2":"8899AABB"}
← {"uid3":"CCDDEEFF"}
```

- This works only in the **~60 s after the device is powered on**. After that the answer is
  `{"ack":"pair_closed"}`.
- Pairing needs no login.
- **key** = `uid0 + uid1 + uid2 + uid3` (32 hex characters). The key stays the same; store it.
  A key of all zeros is not a valid key.

### Login (every connection)

```json
→ {"challenge":1}
← {"nonce0":"01234567"}
← {"nonce1":"89ABCDEF"}
← {"nonce2":"FEDCBA98"}
← {"nonce3":"76543210"}
→ {"resp0":"49fc0325"}      four separate writes, ~100 ms apart
→ {"resp1":"ae9924ee"}
→ {"resp2":"7b9ffb1b"}
→ {"resp3":"6a5d7f4c"}
← {"ack":"auth_ok"}         then the status (section 5)
```

```text
challenge = nonce0 + nonce1 + nonce2 + nonce3           (hex string)
digest    = SHA-256( bytes_from_hex(challenge + key) )  (16 nonce bytes, then 16 key bytes)
hex       = lowercase hex of digest
resp0 = hex[0:8]   resp1 = hex[8:16]   resp2 = hex[16:24]   resp3 = hex[24:32]
```

- Send the four responses as **four separate writes**. If you put them in one JSON object,
  the device does not answer.
- A wrong response gives `{"ack":"auth_fail"}`, and the device disconnects. It
  advertises again at once; repeated failures did not lock the device.

---

## 7. Implementation notes

The ESPHome component in this repository does this:

- One short connection per poll and per command. This saves the device battery, and
  the phone app can still connect.
- Poll: connect → login → read the status → disconnect at the first `sched_en`
  (about 3.5 s).
- Command: connect → login → write the commands → disconnect. Then poll every 10 s until
  `window` reports the target state.
- Find the device by its name, and store its MAC address and the pair key.
- Never resend login messages after a reconnect: the nonces belong to one connection.

---

## 8. Open questions

- meaning of `active`, and of the `auto` / `timer` array slots
- jog commands `3`–`6`, commands `9`–`13`, timer and automation commands (not tested)
- whether the key changes after a factory reset or a new Matter commissioning
- no known way to detect the end of a movement
