# Goodnature BLE protocol reference

Reverse engineered from gateway captures, this gateway's parser, and the [ha-goodnature](https://github.com/codyc1515/ha-goodnature) integration. The A24 examples below came from one Smart Cap running firmware 1.3.0. Treat layouts not marked **observed** as working interpretations that may vary by firmware.

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
| name `GN` with `0x1234` but no C20 marker | A24 (observed on firmware 1.3.0) |
| 9-byte Nordic manufacturer payload with `0x600D` | C20 |

All Goodnature custom UUIDs use the base `0000XXXX-1212-efde-1523-785fef13d123`.

### A24 advertisement observed on firmware 1.3.0

The captured trap used a random BLE address, advertised the local name `GN` padded with NUL bytes, and listed six 16-bit UUID values. It did **not** advertise a 128-bit A24 service UUID. One complete advertising payload was:

```text
02 01 06                         flags
0D 03 44 23 26 97 26 00 34 12 00 00 00 00
                                  complete 16-bit UUID list
05 09 47 4E 00 00                complete local name: GN\0\0
```

The first two UUID slots contain `44 23 26 97`, yielding serial `97262344` when displayed in reverse byte order. The next four slots are **not** stable measurements: another packet from the same trap contained `26 00 34 12 FD D8 00 01`. Their meanings are unknown; do not derive strikes, battery, or firmware from them. `0x1234` in the fourth slot and the `GN` name are useful discovery markers for this profile. A burst of advertisements can indicate activity, but only a GATT read confirms the strike counter.

## C20 advertisement (manufacturer data, 9 bytes)

| Bytes | Meaning |
|---|---|
| 0–3 | serial number, u32 LE, shown as 8 hex digits |
| 4 | device type |
| 5 | flags per the reference integration: `0x01` activated, `0x02` strikes available, `0x08` low battery, `0x10` charging, `0x20` USB connected, `0x40` discoverable. **Unverified**: two Mouse Traps (firmware 0.3.730) that were activated, closed and not charging both sent `0x04`. |
| 6–7 | strike count, u16 LE. Matches the lifetime total the app shows. |
| 8 | documented as battery percent. On the Mouse Trap it is the **battery state** enumeration (`4` = NORMAL) while the UART battery level said 92 %, so the gateway treats it as a state and takes the percentage from the poll. |

Captured Mouse Trap payloads: `477fd0b2 00 04 0e00 04` (serial B2D07F47, 14 strikes) and `8479d0b2 00 04 0300 04` (serial B2D07984, 3 strikes). `tools/gateway_inspect.py decode <hex>` prints the decoded fields.

## A24 GATT (Smart Cap firmware 1.3.0)

Custom short IDs in this section expand into `0000XXXX-1212-efde-1523-785fef13d123`. The following services and characteristics were **observed** in one GATT discovery. `R`, `W`, `N`, and `I` mean read, write, notify, and indicate. Handles can change on other firmware.

| Service | Characteristics: handle (properties) | Gateway interpretation |
|---|---|---|
| `1800` Generic Access | `2A00`: 0003 (RW), `2A01`: 0005 (R), `2A04`: 0007 (R), `2AA6`: 0009 (R) | Standard BLE service |
| `1801` Generic Attribute | `2A05`: 000C (I) | Standard BLE service |
| `E770` | `E771`: 0010 (RWN), `E773`: 0013 (RWN), `E772`: 0016 (RN) | Unknown |
| `FADE` | `FAD1`: 001A (RN), `FAD2`: 001D (RN) | Battery telemetry, units unknown |
| `F1AE` | `F1AF`: 0021 (RWN) | Time |
| `DE11` | `DE12`: 0025 (RN), `DE13`: 0028 (RWN), `DE14`: 002B (RN), `DE15`: 002E (RN), `DE16`: 0031 (RWN), `DE17`: 0034 (RWN) | Identity, control, state, configuration |
| `D00D` | `D20D`: 0038 (RWN), `D30D`: 003B (RN), `D60D`: 003E (RWN), `D50D`: 0041 (RN) | Strike counters and record |
| `DEAD` | `DEED`: 0045 (RN), `D2ED`: 0048 (RWN), `D3ED`: 004B (RWN) | Event channel; details unconfirmed |
| `FE59` | `8EC90003-F315-4F60-9FB8-838830DAEA50`: 004F (WI) | Separate vendor service; purpose unknown |

The table gives characteristic value handles in hex. `N` denotes the advertised notify property; the gateway currently reads these characteristics instead of subscribing.

### Values and decoding

| Characteristic | Observed or implemented interpretation |
|---|---|
| `DE12` | Serial: four binary bytes in reverse display order. `44 23 26 97` → `97262344`. The same four bytes appeared in the advertisement and strike record. |
| `DE15` | Firmware: captured `01 03 00` → `1.3.0`. The parser also accepts `01 03` and ASCII `0103`; those formats were not captured from this trap. |
| `D20D` | Little endian 16-bit strike counter used for **Strikes**. Reads of `1` and later `2` were observed. Whether this counter ever resets remains unverified. Writing the current value back selects a `D30D` record in the gateway's test-fire script. |
| `D60D` | Little endian 16-bit read or acknowledged counter. A read of `1` while `D20D` was `2` produced a pending Kill Alert. The gateway uses `D20D > D60D`. |
| `D30D` | At least 12 bytes. Example: `82 44 23 26 97 12 F7 60 C7 01 01 00`. Bytes 1–4 repeat the serial bytes; byte 5 (`12`) is retained as flags, with individual bits unknown. Bytes 6–9 are a little endian count of minutes since Unix epoch (`2026-09-28 19:03 UTC` in this sample); bytes 10–11 are a little endian strike ID (`1`). Byte 0 (`82`) has unknown meaning. The parser also accepts an ASCII hex representation, not seen in this capture. |
| `FAD1` | One byte in this capture: `D8` (216), and later `E3` (227). The parser also accepts two bytes. Its physical units and a conversion to charge percentage are unverified. |
| `FAD2` | One byte `FD` (253) in this capture. Meaning and units unverified; an earlier reverse engineering label was “internal resistance.” |
| `DE14`, `D50D`, `DE16`, `DE17`, `DEED` | Present in GATT, but their values or bit fields are not decoded here. |
| `F1AF` | Gateway writes Unix time in **minutes**, little endian 32-bit (`floor(epoch seconds / 60)`). |

`D2ED` and `D3ED` are treated as the displayed and acknowledged event counters by the reset script. This interpretation and the meanings of `E770` and `FE59` need more captures. `FAD3` appears in earlier protocol notes but was **absent** from this trap's GATT table.

### Gateway operations

- **Poll:** if enabled and the clock is set, write `F1AF`; then read `DE12`, `DE15`, `D20D`, `FAD1`, `D60D`, `D30D`, `DE14`, `FAD2`, in that order. Missing characteristics are skipped. The A24 sleeps and may stop accepting connections before the poll finishes.
- **Clear Kill Alert:** read `D20D` and `D2ED`; copy their values to `D60D` and `D3ED` respectively; write `02` to `DE13`; then poll. These writes are the gateway's implemented acknowledgement sequence, not a fully verified on-air trace of the app.
- **Test Fire:** write `05` to `DE13`, wait 500 ms, read `D20D`, write that value back to `D20D`, read `D30D`, then poll. **This command fires the trap.** The sequence is implemented in the gateway but was not exercised on the captured trap.

### Battery and contact state

No charge percentage or low-battery threshold has been established for the observed `FAD1`/`FAD2` values. Without user-supplied raw calibration, the gateway leaves A24 **Battery %** and **Battery Low** unavailable. Its **Battery Status** text uses contact as a proxy: `Normal` if the gateway received an advertisement within the past 24 hours, `Unknown` otherwise. It does not measure charge or observe a sync performed only by the Goodnature app. **Last Seen** records the gateway's latest advertisement; a successful GATT poll is a separate event.

No A24 characteristic in this capture reported CO₂ shots remaining. The gateway calculates that Home Assistant value from its configured canister capacity, a stored baseline for `D20D`, and any manual **CO₂ Shot Used** adjustments. The baseline is established when the counter first becomes known or when the user marks a new canister.

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
