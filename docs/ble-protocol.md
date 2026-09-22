# Goodnature BLE protocol reference

Condensed from the [ha-goodnature](https://github.com/codyc1515/ha-goodnature) integration (`protocol.py`, `coordinator.py`). Everything here is reverse engineered and may vary by firmware.

## Discovery

A trap advertisement is recognised by any of:

- local name `GN`
- a Goodnature 128-bit service UUID: `D00D`, `DE11`, `FADE`, `E010` (base below)
- legacy 16-bit service UUIDs `0x1234` or `0x600D`
- Nordic manufacturer data (company id `0x0059`) together with one of the above

Model classification:

| Marker | Model |
|---|---|
| `E010`, Nordic UART service, or Memfault service in the advertisement | C20 |
| `D00D` or `D2ED` | A24 |
| 9-byte Nordic manufacturer payload with `0x600D` | C20 |

All Goodnature custom UUIDs use the base `0000XXXX-1212-efde-1523-785fef13d123`.

## C20 advertisement (manufacturer data, 9 bytes)

| Bytes | Meaning |
|---|---|
| 0–3 | serial number, u32 LE, shown as 8 hex digits |
| 4 | device type |
| 5 | flags per the reference integration: `0x01` activated, `0x02` strikes available, `0x08` low battery, `0x10` charging, `0x20` USB connected, `0x40` discoverable. **Unverified**: two Mouse Traps (firmware 0.3.730) that were activated, closed and not charging both sent `0x04`. |
| 6–7 | strike count, u16 LE. Matches the lifetime total the app shows. |
| 8 | documented as battery percent. On the Mouse Trap it is the **battery state** enumeration (`4` = NORMAL) while the UART battery level said 92 %, so the gateway treats it as a state and takes the percentage from the poll. |

Captured Mouse Trap payloads: `477fd0b2 00 04 0e00 04` (serial B2D07F47, 14 strikes) and `8479d0b2 00 04 0300 04` (serial B2D07984, 3 strikes). `tools/gateway_inspect.py decode <hex>` prints the decoded fields.

## A24 GATT

| Short UUID | Role | Format |
|---|---|---|
| `DE11` | device information service | |
| `DE12` | serial number | string |
| `DE13` | device control | u8, writable. `0x02` acknowledge, `0x05` test fire |
| `DE14` | device state | raw u16 |
| `DE15` | firmware version | string |
| `DE16` | device config | raw |
| `D00D` | kill service | |
| `D20D` | kill displayed counter | u16 LE, also written back to select a strike record |
| `D30D` | strike record | 12+ bytes: `[5]` flags, `[6..9]` minutes since epoch u32 LE, `[10..11]` strike id u16 LE. Sometimes ASCII-hex encoded |
| `D50D` | kill state | raw u16 |
| `D60D` | kill read pointer | u16 LE, writable |
| `D2ED` | event displayed counter | u16 LE |
| `DEED` | event payload | raw |
| `D3ED` | event read pointer | u16 LE, writable |
| `FADE` | battery service | |
| `FAD1` | battery voltage | raw u16 |
| `FAD2` | battery internal resistance | raw u16 |
| `FAD3` | unknown | raw u16 |
| `F1AE` | time service | |
| `F1AF` | time | u32 LE minutes since epoch, writable |

Flows used by this gateway:

- **Poll**: write `F1AF` = now/60; read `DE12`, `DE15`, `DE14`, `D20D`, `D60D`, `D30D`, `FAD1`, `FAD2`.
- **Reset alert**: read `D20D`, `D2ED`; write `D60D` = kill displayed, `D3ED` = event displayed; write `DE13` = `0x02`; then poll.
- **Test fire**: write `DE13` = `0x05`; wait 500 ms; read `D20D`; write it back to `D20D`; read `D30D`; then poll.

Kill alert for the A24 is `D20D > D60D`.

## C20 Nordic UART

Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, write to RX `6e400002…`, notifications from TX `6e400003…`.

Frame: `B0 <escaped body> B1`. Body: `type, subtype, payload…, crc16 LE`. CRC16-CCITT, polynomial `0x1021`, init `0xFFFF`, over `type + subtype + payload`. Inside the body `B0`/`B1` are sent as `B2 (byte ^ 0x04)`; this implementation escapes `B2` the same way.

| Type | Name | Request | Response payload |
|---|---|---|---|
| `0x04` | set time | subtype `0x02`, u32 LE epoch seconds | |
| `0x08` | firmware | subtype `0x00` | NUL-terminated string |
| `0x10` | device state | subtype `0x00` | `[0..3]` time, `[4]` device state, `[5]` kill state, `[6]` tray, `[7]` battery state, `[8]` charge state, `[9]` USB state, `[10..13]` strike count u32. On the Mouse Trap this count read `0` while the advertisement said `14`, so it is not the lifetime total (probably kills since the last clear); the gateway ignores it when an advertisement count exists. |
| `0x11` | battery level | subtype `0x00` | `[0..3]` time, `[4]` percent |
| `0x12` | set command | subtype `0x02`, u32 LE command | response subtype `0x01`; async `0x10` and `0x31` follow |
| `0x31` | striker event | (unsolicited, and replayed inside `0x45`) | `[0..3]` time, `[4..7]` "strike count" (read `8` on a trap whose log holds one kill; meaning unknown), `[8]` source, `[9..12]` trigger number, `[13..16]` fire, `[17..20]` rewind, `[25..28]` backdrive. The durations read 31128 / 306000 / 15000, which only make sense as microseconds. **Kills = number of TRIGGER events in the log**; that is the one figure that matched the app (1 and 0 for two traps advertising 14 and 3). |
| `0x40` | logs request | subtype `0x00`, u32 start, u32 end, u32 flags (`1<<1` striker events) | LogsResponse (`start, end, flags` echoed) terminates the stream. The trap scans its log at a fixed pace: a 24 h window answers in ~0.5 s, a window from 2017 takes ~12 s, and a start of `0` streamed the events but never sent the terminator. The gateway waits up to 25 s. It replays from 2017 when the count is unknown, once a day and on Poll Now; otherwise it asks only for events since the last completed window, minus a 2 min overlap, and ignores TRIGGER events at or before the last known strike time. |
| `0x45` | log event | (streamed) | payload is a nested frame, e.g. a `0x31` striker event |

Responses have subtype `0x01`.

Commands: FIRE = `1049702590` (`0x3E9130BE`), CLEAR = `1682499629` (`0x644A8C2D`).

Enumerations (index → meaning):

- device state: DEACTIVATED, ACTIVATED, ERROR, NO_DATA
- kill state: CLEARED, DETECTED, NO_DATA
- tray: OPEN, CLOSED, UNKNOWN, NO_DATA
- battery state: STARTUP, NOT_CONNECTED, CRITICAL, LOW, NORMAL, NO_DATA
- charge state: NOT_CHARGING, CHARGING, NO_DATA
- USB: DISCONNECTED, CONNECTED, NO_DATA
- striker source: TRIGGER, UNUSED, USER, NO_DATA (USER = manual test fire)

Poll sequence used here: subscribe TX, set time, request `0x08`, `0x11`, `0x10`, then a logs request for striker events (full or incremental, see above).

## Capturing data from the gateway

The quickest way is `make capture` (or `tools/gateway_inspect.py capture --poll`), which turns Debug Mode on over the API, streams the log and the raw capture sensors with the frames decoded inline, and turns Debug Mode off again. `tools/gateway_inspect.py decode` decodes pasted values offline. Underneath, with the gateway's **Debug Mode** switch on:

- every Goodnature-like advertisement is logged as `ADV <why> [MAC] rssi= type= name= / services=[...] / mfg=[company:hex] / raw adv=<hex> scan_rsp=<hex>`
- every connection logs the GATT table with handles and properties (`R` read, `W` write, `w` write without response, `N` notify, `I` indicate)
- each trap's **Last Advertisement** text sensor holds `adv|scan_rsp` hex and **Last Frame** holds `UUID:hex` (A24 read) or `UART type/subtype:hex` (C20 frame)

Please attach those when reporting a trap that is not recognised or a read that fails.
