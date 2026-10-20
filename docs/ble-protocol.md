# Goodnature BLE protocol reference

Reverse engineered from gateway captures, this gateway's parser, the [ha-goodnature](https://github.com/codyc1515/ha-goodnature) integration, the decompiled Goodnature Android app (3.5.3 and 3.6.1) and the strings in the Mouse Trap firmware images it bundles (0.2.570 and 0.3.730). The A24 examples below came from one Smart Cap running firmware 1.3.0. Treat layouts not marked **observed** as working interpretations that may vary by firmware; "the app" below means those app versions.

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

The six 16-bit values are data, not services. The app accepts an advertisement only with exactly six; read as little-endian words:

| Slot | Contents |
|---|---|
| [0], [1] | serial: `44 23 26 97` → `97262344` (shown in reverse byte order) |
| [2] | low byte flags: `0x01` strikes available, `0x02` activated, `0x04` moving, `0x08` low battery, `0x10` output power, `0x20` new event, `0x40` new error. High byte device type. |
| [3] | `0x1234`, a marker |
| [4] | high byte battery voltage, `V = raw / 255 × 3.6`; low byte battery internal resistance (raw, units unknown). `0x0000` before the battery has been measured. |
| [5] | high byte kill count (low 8 bits of `D20D`), low byte unread kills (not yet acknowledged on the trap) |

The second captured packet, `26 00 34 12 FD D8 00 01`, decodes as flags `0x26` (activated, moving, new event), battery 3.05 V with internal resistance 253, one kill and none unread; it matches `FAD1` `D8` and `FAD2` `FD` read over GATT. The first packet was sent before any strike. The app reads its A24 battery only from here, never over GATT, and uses the flags and kill counts only for logging. `tools/gateway_inspect.py decode` decodes an `ADV` log line or a **Last Advertisement** value.

## C20 advertisement (manufacturer data, 9 bytes)

| Bytes | Meaning |
|---|---|
| 0–3 | serial number, u32 LE, shown as 8 hex digits |
| 4 | device type |
| 5 | flags per the reference integration: `0x01` activated, `0x02` strikes available, `0x08` low battery, `0x10` charging, `0x20` USB connected, `0x40` discoverable. **Observed** on two Mouse Traps (firmware 0.3.730), only partly matching: `0x02` tracks the kill alert (`0x06` with an alert, `0x04` about 6 s after SetCommand(CLEAR), still `0x04` after a power cycle). `0x10` tracks charging: `0x34` while the poll said CHARGING, `0x24` once it said NOT_CHARGING. `0x20` was set throughout that charge, consistent with USB connected. Activated traps send `0x04`, never `0x01`; `0x04` is not in the reference layout. `0x08` and `0x40` not yet seen. The app uses `0x08` for its background low-battery check, `0x10`/`0x20` only to filter traps when adding one, and logs `0x02`. |
| 6–7 | strike count, u16 LE. Matches the lifetime total the app shows. |
| 8 | documented as battery percent. On the Mouse Trap it is the **battery state** enumeration (`4` = NORMAL) while the UART battery level said 92 %, so the gateway treats it as a state and takes the percentage from the poll. |

Captured Mouse Trap payloads: `477fd0b2 00 04 0e00 04` (serial B2D07F47, 14 strikes) and `8479d0b2 00 04 0300 04` (serial B2D07984, 3 strikes). `tools/gateway_inspect.py decode <hex>` prints the decoded fields.

## A24 GATT (Smart Cap firmware 1.3.0)

Custom short IDs in this section expand into `0000XXXX-1212-efde-1523-785fef13d123`. The following services and characteristics were **observed** in one GATT discovery. `R`, `W`, `N`, and `I` mean read, write, notify, and indicate. Handles can change on other firmware.

| Service | Characteristics: handle (properties) | Gateway interpretation |
|---|---|---|
| `1800` Generic Access | `2A00`: 0003 (RW), `2A01`: 0005 (R), `2A04`: 0007 (R), `2AA6`: 0009 (R) | Standard BLE service |
| `1801` Generic Attribute | `2A05`: 000C (I) | Standard BLE service |
| `E770` | `E771`: 0010 (RWN), `E773`: 0013 (RWN), `E772`: 0016 (RN) | Error channel. App names: `E771` Error Displayed, `E772` Error Data, `E773` Error Read. The app never reads them; the gateway reads them for diagnostics (**Last Error Data**). `E771` is a cursor like `D20D`: `E772` read all zero until `E771` was written back, then returned the record. **Observed** record, 17 bytes: `02 00 D2 DC C7 01 64 01 5C 79 0D 00 00 00 35 09 01` = `[0..1]` record number (2), `[2..5]` minutes since the Unix epoch (2026-10-20 19:30 UTC), `[6..9]` the trap's ID (serial `795C0164`), then `[10..13]` u32 `13`, `[14..15]` u16 `2357`, `[16]` `01`. The other A24's first record had `0x20005994` in `[10..13]` (an nRF52 RAM address) and the same `2357` and `01`, which may be a source line and file ID. Every record on both traps was created in a session that ended with the trap hanging up on the `02` write, after `E771` had been read unchanged earlier in the session; sessions the gateway closed itself created none. Both traps had no records before the gateway began writing `02` every session, although both had been synced with the app. The new-error flag cleared in the advertisement after the next session in which the gateway sent `02` and disconnected first, with nothing written to `E773`. Writing `E773` (copying `E771`) was accepted but did not clear it while every session still ended in a hang-up, and in 9 sessions without `02` it stayed set. **Trap Error** comes from these records, not the advertisement's flag: each session the gateway reads the newest record, ignores the session-end kind (`[14..15]` = 2357, `[16]` = 1), and latches **Trap Error** on for any other kind until **Clear Trap Error**. Only the newest record is read, so of several new ones only the last is judged. |
| `FADE` | `FAD1`: 001A (RN), `FAD2`: 001D (RN) | Battery. App names: `FAD1` Battery Voltage, `FAD2` Battery Internal Resistance (see below) |
| `F1AE` | `F1AF`: 0021 (RWN) | Time |
| `DE11` | `DE12`: 0025 (RN), `DE13`: 0028 (RWN), `DE14`: 002B (RN), `DE15`: 002E (RN), `DE16`: 0031 (RWN), `DE17`: 0034 (RWN) | App names: `DE12` Device ID, `DE13` Device Control, `DE14` Device State, `DE15` Device Version. `DE16`, `DE17` unnamed. |
| `D00D` | `D20D`: 0038 (RWN), `D30D`: 003B (RN), `D60D`: 003E (RWN), `D50D`: 0041 (RN) | App names: `D20D` Kill Displayed, `D30D` Kill Data, `D50D` Kill State, `D60D` Kill Read |
| `DEAD` | `DEED`: 0045 (RN), `D2ED`: 0048 (RWN), `D3ED`: 004B (RWN) | Event channel. App names: `DEED` Event Data, `D2ED` Event Displayed, `D3ED` Event Read |
| `FE59` | `8EC90003-F315-4F60-9FB8-838830DAEA50`: 004F (WI) | Separate vendor service; purpose unknown |

The table gives characteristic value handles in hex. `N` denotes the advertised notify property; the gateway currently reads these characteristics instead of subscribing.

### Values and decoding

| Characteristic | Interpretation (app = Goodnature app 3.5.3 / 3.6.1) |
|---|---|
| `DE12` | Serial: four binary bytes in reverse display order. `44 23 26 97` → `97262344`. The same four bytes appear in the advertisement and the kill record. |
| `DE15` | Firmware: each byte as a decimal, joined with `.`: captured `01 03 00` → `1.3.0`. |
| `DE13` | Control. `0x02` ends a sync: the app writes it last, then disconnects. Without it the trap keeps its strikes-available, new-event and new-error flags set and keeps waking (seen every 15–60 s for over ten minutes). The trap hangs up a couple of connection events after `0x02` and logs an error record when it gets there before the central's disconnect (see `E770`). `0x05`: the app writes it during setup on every `D2ED` (movement) notification; written to two A24s, it fired neither. The Smart Cap only counts strikes, and the app's A24 test fire is a stick on the trigger. `0x00` activates a new trap, `0x04` is part of the app's old setup flow. |
| `D20D` | u16 LE, the number of the newest kill record, used for **Strikes**. It is also a cursor: writing a record number selects what `D30D` returns. Reads of `1` and later `2` were observed; the app has no handling for a reset. |
| `D30D` | The selected kill record, 12 bytes: `[0]` peak level, `[1..4]` the trap's ID (the serial bytes), `[5]` air temperature (signed, °C), `[6..9]` minutes since the Unix epoch (u32 LE), `[10..11]` record number (u16 LE, 0 = no record). Example `82 44 23 26 97 12 F7 60 C7 01 01 00`: record 1 at `2026-09-28 19:03 UTC`, 18 °C. The parser also accepts an ASCII-hex form, not seen in a capture. |
| `D60D` | u16 LE, the last record acknowledged. The app writes the highest record it read here at the end of every sync. |
| `D2ED` / `D3ED` | u16 LE movement-event counter (stored by the app as a movement count) and its read pointer, which the app reads but never writes. |
| `D50D` | Kill state: 0 idle, 1 wait, 2 recording, 3 buffer. Only the app's old setup flow subscribes to it. |
| `FAD1` | Battery voltage, one byte: `V = raw / 255 × 3.6` (216 → 3.05 V, 227 → 3.20 V). The same byte as advertisement slot [4]. |
| `FAD2` | Battery internal resistance, one byte, units unknown. The app rates the battery from it: under 56 Full, under 200 Medium, under 230 Low, otherwise Critical. The capture read 253 (Critical) from a working trap. |
| `F1AF` | Unix time in **minutes**, little endian 32-bit (`floor(epoch seconds / 60)`), written by the gateway and the app on connect. |
| `DE14`, `DE16`, `DE17`, `DEED`, `E771`–`E773` | Never read or written by the app. The gateway reads `DE14`, `E771`, `E772` and `E773` for diagnostics only. |

`FE59` needs more captures. `FAD3` appears in earlier protocol notes but was **absent** from this trap's GATT table.

### How the app syncs an A24

Verified in app 3.6.1 (`ud.c`, `ud.d`, `sd.n`, `sd.t`, `Od.m1`):

- **Link:** `connectGatt` over LE without auto-connect, then high connection priority (Android: 11.25–15 ms interval). Every read, write and descriptor write to a trap starts at least 300 ms after the previous one and times out after 1 s. Writes to `DE13` wait for the confirmation. The app does not reconnect to a trap within 6 s of disconnecting (21 s before Android 10).
- **Routine sync** (`ud.c` → `ud.d`): read `D20D`, then `DE15`, `D2ED` and `D60D`. For each `n` from `D60D + 1` up to `D20D`: write `n` to `D20D`, read `D30D`, write `n` to `D60D`. Then write `F1AF`, write `0x02` to `DE13`, and disconnect as soon as that write completes. It subscribes to nothing. If anything fails it skips `F1AF` and `0x02` and disconnects.
- **Setup** (`Od.m1`): write `0x04` to `DE13`, write `F1AF`, read `DE15`, write `0x00` to `DE13`, subscribe to `D2ED`, `D50D` and `D20D`, wait for a `D20D` value, run the same record walk, then disconnect. On every `D2ED` (movement) notification during setup it writes `0x05` to `DE13`; whether `0x05` fires the trap is not settled.

An earlier note here had the app retry each read three times 1.2 s apart, subscribe to `D20D` only, walk the records backwards and disconnect a second after `0x02`. The 3.6.1 paths above do none of these.

The app never wakes an A24: it only syncs when the trap wakes, after a strike, when its battery goes in, or when it is shaken until the red light flashes ("Your Smart Cap only syncs automatically when it has strike data to share").

### Gateway operations

- **Poll:** if enabled and the clock is set, write `F1AF`; read `DE12`, `DE15` and `D20D`; write `D20D` back (selecting the newest record, unless there are none); read `D30D`, `D60D`, `FAD1`, `FAD2`, `DE14`, `E771`; write `E771` back (unless it is 0; a refused write does not fail the poll); read `E773`, `E772`; then acknowledge like the app: copy `D20D` to `D60D` (unless there are no kills) and write `02` to `DE13`. Missing characteristics are skipped. The A24 sleeps and may stop accepting connections before the poll finishes; the acknowledgement then waits for the next session. **Observed**: the trap hangs up (HCI reason `0x13`, remote user terminated) a couple of connection events after `02`, and when that comes before the gateway's disconnect the write may never be confirmed (status 133, which the gateway counts as the end of the session) and the trap adds an error record. At ESP-IDF's default 30–50 ms interval the trap hung up first in 4 of 8 sessions; at the app's 11.25–15 ms, in 4 of 4. Each of the 6 hang-ups since checked had added an error record; the sessions the gateway closed first added none. Leaving `02` out avoided the hang-ups (9 of 9 sessions closed by the gateway, no new records) but left the flags set and the trap waking. A 75–100 ms interval did not stop it either (a new record in the first session) and made connection setup take 21 s instead of 5–9 s, so the gateway uses ESP-IDF's default and **Trap Error** ignores these records.
- **Kill Alert** is the gateway's own: it turns on when **Strikes** goes up, when a poll finds `D20D` ahead of `D60D` (kills nobody acknowledged), or when an advertisement shows unread kills, and it stays on until **Clear Kill Alert**, which only clears it in the gateway. Because every session acknowledges, the trap stops waking to report the kills; the Goodnature app still downloads them, but without its own "new strike" notification.
- **Test Fire** is not offered for an A24: `05` to `DE13` fired neither of two traps (see `DE13`). A test fire by hand is an ordinary strike to the gateway.

### Battery and contact state

**Battery Voltage** comes from `FAD1` or the advertisement, and **Battery Status** is the app's internal-resistance rating. **Battery Low** follows the advertisement's low-battery flag rather than that rating, because the one capture read Critical from a working battery. The app has no percentage for an A24; the optional `a24_battery_*_raw` calibration still provides one from the raw voltage byte. The A24 advertises when switched on, after a kill and when shaken, and is otherwise silent, so the gateway treats it as present, and **Online**, from its first check-in until it is forgotten. **Last Seen** records the gateway's latest advertisement; a successful GATT poll is a separate event.

No A24 characteristic reports CO₂ shots remaining. Like the app, the gateway counts down from the canister capacity (24): one shot per strike since the canister was replaced, including test fires, plus one shot lost for every 21 days in the trap (`co2_leak_days`), plus any **CO₂ Shot Used** adjustments. The app rates 12 or more Fresh, 5 or more Ok, 1–4 Low and 0 Replace; **CO₂ Low** is on at 4 or fewer (`co2_low_threshold`). The paste lasts 180 days in the app (`lure_life_days`).

## C20 Nordic UART

Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, write to RX `6e400002…`, notifications from TX `6e400003…`.

Frame: `B0 <escaped body> B1`. Body: `type, subtype, payload…, crc16 LE`. CRC16-CCITT, polynomial `0x1021`, init `0xFFFF`, over `type + subtype + payload`. Inside the body `B0`/`B1` are sent as `B2 (byte ^ 0x04)`; this implementation escapes `B2` the same way. The app escapes only `B0`/`B1` but, receiving, treats any `B2` as an escape, so a literal `B2` it sends would be misread by a decoder like its own; escaping it is the safe choice. The CRC is CRC-16/CCITT-FALSE.

| Type | Name | Request | Response payload |
|---|---|---|---|
| `0x04` | set time | subtype `0x02`, u32 LE epoch seconds | |
| `0x07` | hardware build target | | NUL-terminated string (the 0.2.570 image names itself `c20_dts_v2`). The app decodes it; no request for it was found. |
| `0x08` | firmware | subtype `0x00` | NUL-terminated string |
| `0x10` | device state | subtype `0x00` | `[0..3]` time, `[4]` device state, `[5]` kill state, `[6]` tray, `[7]` battery state, `[8]` charge state, `[9]` USB state, `[10..13]` strike count u32. On the Mouse Trap this count read `0` while the advertisement said `14`, so it is not the lifetime total (probably kills since the last clear). The app never reads it, and neither does the gateway. |
| `0x11` | battery level | subtype `0x00` | `[0..3]` time, `[4]` percent |
| `0x12` | set command | subtype `0x02`, u32 LE command | response subtype `0x01`; async `0x10` and `0x31` follow. The app does not wait for the response. |
| `0x13` | activation key | subtype `0x00` | 16 bytes, a UUID (two big endian u64). The app reads it when activating a trap. |
| `0x14` | kill history | subtype `0x00` | ten u32 kill times, epoch seconds, `0` for an empty slot. On firmware after 0.2.550 the app counts kills from this, asking at connect and whenever the device state says DETECTED. |
| `0x30` | trigger event | (replayed inside `0x45` with log flag bit 0) | `[0..3]` time, `[4..7]` count; from firmware 0.1.276 the infrared readings, u32 each: inner sensor ambient `[8]`, on reading `[12]`, baseline `[16]`, threshold `[20]`, on time `[24]`; outer sensor ambient `[28]`, on reading `[32]`, baseline `[36]`, threshold `[40]`, on time `[44]`; alert time `[48]`. The baselines are IEEE 11073 32-bit FLOATs (signed 24-bit mantissa × 10 ^ signed 8-bit exponent, the high byte). The gateway asks for these only around new kills (see below). |
| `0x31` | striker event | (unsolicited, and replayed inside `0x45`) | `[0..3]` time, `[4..7]` "strike count" (read `8` on a trap whose log holds one kill; meaning unknown), `[8]` source, `[9..12]` trigger number, `[13..16]` fire, `[17..20]` rewind, `[21..22]` peak fire current and `[23..24]` peak rewind current (i16 LE, mA per the firmware's log strings; from 0.1.276), `[25..28]` backdrive (from 0.1.403). The durations read 31128 / 306000 / 15000, which only make sense as microseconds. TRIGGER events in the log matched the app's kill total (1 and 0 for two traps advertising 14 and 3). Sources (app 3.6.1): 0 SENSOR, 1 UNUSED, 2 APP (test fire), 3 BACKDRIVE_TEST (a motor self-test), 4 UNKNOWN; only SENSOR is a kill. On firmware after 0.2.550 the app takes kills from `0x14` instead and uses live striker events only for its test fires. |
| `0x40` | logs request | subtype `0x00`, u32 start, u32 end, u32 flags | Flag bits per the app: 0 trigger events, 1 striker events, 2 errors, 3 warnings, 4 general events, 5 device status log, 6 debug info. The gateway asks only for striker events. LogsResponse (`start, end, flags` echoed) terminates the stream. The trap scans its log at a fixed pace: a 24 h window answers in ~0.5 s, a window from 2017 takes ~12 s, and a start of `0` streamed the events but never sent the terminator. The gateway waits up to 25 s. On firmware after 0.2.550 it replays from 2017 only when the count is unknown and once per Poll Now press. On older firmware it also replays once a day, and otherwise asks only for events since the last completed window, minus a 2 min overlap, ignoring TRIGGER events at or before the last known strike time. |
| `0x45` | log event | (streamed) | payload is a nested frame, e.g. a `0x31` striker event |
| `0x62` | input states | subtype `0x00` | `[0]` charge pin, `[1]` USB pin, `[2]` button pin, `[3]` battery error pin, `[4..5]` battery voltage (i16 LE, mV, `0` = not measured), `[6..7]` motor feedback (i16 LE, units unknown). App 3.6.1 asks with every battery poll, without waiting for an answer. **Observed**: two Mouse Traps on 0.3.730 never answered it in normal operation. The firmware has a "pin states" handler; its handlers also serve a factory interface (`factoryIn` / `factoryOut`), which may be the only place it answers. |
| `0xF8` | bootloader version | subtype `0x00` | NUL-terminated string. App 3.6.1 asks at connect. **Observed** `2.0.0` (`322e302e30`) on two Mouse Traps on 0.3.730. |

Responses have subtype `0x01`.

Commands, sent as the u32 in a `0x12` request:

| Command | Value | Use |
|---|---|---|
| FIRE | `0x3E9130BE` (1049702590) | Test fire. The app then waits for the `0x31` striker event. |
| CLEAR | `0x6448E82D` (1682499629) | Clear the kill alert. Sent from the app's trap screen. |
| WAKE | `0x528670E4` (1384542436) | App 3.6.1 sends it first in every session, and again when the device state shows a trap that needs battery recovery. The gateway sends it first too. **Observed**: acknowledged with a `0x12` response whose payload echoes the command. |
| ACTIVATE | `0xE92F9A13` (3912210963) | Defined in the app but never sent by it. Not used by the gateway. |
| DEACTIVATE | `0x2AFC58D4` (721180884) | Defined in the app but never sent by it. **Observed** on 0.3.730 (tray closed, armed): acknowledged with `0x12`, but the device state stayed ACTIVATED, also on a poll 90 s later, and the trap kept advertising. Not used by the gateway. |

The app also names SLEEP, IDENTIFY, OFF and RESTART, without values.

Enumerations (index → meaning):

- device state: DEACTIVATED, ACTIVATED, ERROR, NO_DATA
- kill state: CLEARED, DETECTED, NO_DATA
- tray: OPEN, CLOSED, UNKNOWN, NO_DATA
- battery state: STARTUP, NOT_CONNECTED, CRITICAL, LOW, NORMAL, NO_DATA
- charge state: NOT_CHARGING, CHARGING, NO_DATA
- USB: DISCONNECTED, CONNECTED, NO_DATA
- striker source: TRIGGER, UNUSED, USER, BACKDRIVE_TEST, NO_DATA (app 3.6.1 names: SENSOR, UNUSED, APP, BACKDRIVE_TEST, UNKNOWN; USER = manual test fire)

Poll sequence used here, in app 3.6.1's order: subscribe TX, WAKE, set time, request `0xF8`, `0x08`, `0x10`, `0x11`, `0x62`, `0x14`, then a logs request when one is due (see below). A test fire, clear, arm or disarm command goes right after WAKE. The app spaces its requests 50 ms apart without waiting; the gateway waits for each answer. WAKE, `0xF8`, `0x62` and `0x14` are optional for older firmware: no answer within 4 s does not fail the poll, and the gateway stops sending that request to the trap until its firmware version changes or the gateway restarts. `0x14` is skipped on firmware 0.2.550 and older, and a trap that ignores it falls back to counting from the log.

**Gateway kill counting.** Each poll counts the `0x14` entries newer than the newest one counted before; the gateway keeps that time in flash, and it never moves backwards, so the empty history after a factory reset is harmless. The first read is the baseline, because those kills are already in the total. Only SENSOR (TRIGGER) striker events count as kills. The striker log is replayed only for the lifetime total: when it is unknown (a newly found trap), and on Poll Now. If that replay does not finish, the number of `0x14` entries stands in as a lower bound (exact up to ten kills). The logs request is planned once this session's `0x14` answer is in. When the history holds new kills, the gateway asks for striker and trigger events (flags `0b11`) from 2 minutes before the oldest new kill. That short window answers quickly and gives the kills' motor currents and infrared readings, but it is not used for counting.

Two Mouse Traps (0.3.730) that had been polled for days started answering the TX CCCD write with ATT error `0x11` (Insufficient Resources) and kept doing so after a power cycle; the Goodnature app could not connect to them either. A factory reset fixed both. Writes to RX were still acted on (a CLEAR changed the advertisement flags). The app pairs with a trap when it reads Memfault diagnostics (service `54220000-f6a5-4007-a371-722f4ebd8436`, which the user opts into), and the firmware keeps pairings and settings in flash, which a factory reset clears and a power cycle does not. That fits subscription slots used up by stored pairings, but it is not confirmed, and app 3.6.1 has no Memfault code and never pairs. When the subscription fails the gateway skips every request, still writes a FIRE or CLEAR command, and ends the session.

### How the app reads a Mouse Trap

- **Connection:** BALANCED connection priority, no MTU request (23 bytes, so frames span notifications), notifications on TX, write-with-response to RX. A 10 s connect timeout and 10 s per command. While connected it asks for the battery level every 60 s. Its background scan runs every 15 minutes and connects to every trap it owns.
- **Status**, highest first: device state ERROR → *Error* ("Restart your trap by holding down the button until it starts flashing white"); CHARGING → *Charging*; USB connected but not charging → *Charged* ("Remove the trap from the charger to set it"); kill DETECTED → *New strike*, with an **Empty trap** button that sends CLEAR; tray OPEN → *Trap open* ("Close the trap to set it"); otherwise *Set*. ACTIVATED / DEACTIVATED is never shown.
- **Battery:** Unknown before a reading, then Charging, Charged, or by level: 85 % and up Full, 45 % Good, 15 % Ok, 5 % Low, below that Empty. It notifies "Low battery" at 20 % or below, or when the advertisement's `0x08` bit is set, once until a reading is no longer low.
- **Test fire** is offered only when the trap is connected, off USB, the kill state is not DETECTED, the tray is not OPEN and the device state is not ERROR. It sends FIRE and waits up to 10 s for the APP striker event.
- **Donut** (the lure) lasts 90 days: fresh above half, "good" above a third, "stale" above 6 %, then "replace".
- **Kills** are the non-test strikes: KillHistory entries newer than the newest one saved (older ones are skipped), plus live APP events as tests.

The gateway follows these: **Trap State**, **Trap Error**, **Armed** (= *Set*), **Battery Status**, **Battery Low**, the Test Fire conditions and a 90-day default lure life.

### Battery recovery (firmware 0.3.730 with bootloader 2.0.0)

App 3.6.1 bundles firmware 0.3.730 and a second image that replaces bootloader 1.0.0 with 2.0.0 (the app: "This second update is critical for battery performance"); the updater leaves any other bootloader alone. With bootloader 2.0.0 the trap runs a one-time **battery recovery** at power-up while on USB power. Per the firmware's log strings:

1. The bootloader starts a PI loop that drives the motor PWM to hold the battery voltage at a target (`VBAT: … mV, Target: … mV, … Duty: …`). It skips the loop if the battery is fine, recovery is already done, or the trap is not charging.
2. The application firmware takes over (`Handover from bootloader: duty …, I-error …`).
3. It finishes when the voltage stays below the target for a number of cycles. Otherwise it stops after a maximum run time, ramping the target down near the end; the app says this takes up to 5 minutes.
4. The trap records that the battery has been recovered and skips the process from then on. With bootloader 1.0.0 it logs `Bootloader v1.0.0 detected, skipping battery recovery`.

No BLE command starts it. The app offers it when a trap reports battery 0 %, charge state NOT_CHARGING (or NO_DATA) and USB CONNECTED. Its instructions: unplug the trap, wait 5 seconds, plug it back in, and wait for the light to pulse white and pink; if it flashes once and turns off, or stops pulsing, repeat. The app's **Start battery recovery** only watches: it polls `0x11` and `0x62` every 5 s, treats CHARGING as recovering and a level above 0 % as done, and gives up if the trap stays disconnected for 3 minutes after charging was seen.

### Firmware updates

The app updates the Mouse Trap with MCUmgr over SMP (service `8d53dc1d-1db7-4cd3-868b-8a527460aa84`), using images bundled in the app: validate, upload, test, reset, confirm. After an update it reconnects and runs the normal session. The gateway does not update firmware.

## Capturing data from the gateway

The quickest way is `make capture` (or `tools/gateway_inspect.py capture --poll`), which turns Debug Mode on over the API, streams the log and the raw capture sensors with the frames decoded inline, and turns Debug Mode off again. `tools/gateway_inspect.py decode` decodes pasted values offline. Underneath, with the gateway's **Debug Mode** switch on:

- every Goodnature-like advertisement is logged as `ADV <why> [MAC] rssi= type= name= / services=[...] / mfg=[company:hex] / raw adv=<hex> scan_rsp=<hex>`
- every connection logs the GATT table with handles and properties (`R` read, `W` write, `w` write without response, `N` notify, `I` indicate)
- each trap's **Last Advertisement** text sensor holds `adv|scan_rsp` hex and **Last Frame** holds `UUID:hex` (A24 read) or `UART type/subtype:hex` (C20 frame)

Please attach those when reporting a trap that is not recognised or a read that fails.
