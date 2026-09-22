# ESPHome Goodnature BLE Gateway

Monitor [Goodnature](https://goodnature.co.nz) A24 Smart Traps and Mouse Traps in Home Assistant with an ESP32-C6 running [ESPHome](https://esphome.io). Put the gateway near your traps and it discovers them, tracks strikes, battery and consumables, and exposes **each trap as its own Home Assistant device**. Fully local: no cloud, no app.

> **Status: experimental.** The protocol layer is ported from the
> [ha-goodnature](https://github.com/codyc1515/ha-goodnature) integration and
> covered by host-side unit tests, but the gateway has had little time against
> real traps and no real kill has been observed through it yet. Issues with
> debug captures are welcome.

## How it works

Goodnature traps use Bluetooth Low Energy. This project reimplements the app's protocol as a native ESPHome component: one **hub** owns a fixed number of **trap slots**, and every slot holding a trap is a Home Assistant sub-device.

- **Discovery.** The hub recognises Goodnature traps from their advertisements and binds each to the first free slot. Bindings are stored in flash, so a trap keeps its device across reboots. A Mouse Trap is tracked by the serial in its advertisement, an A24 by its Bluetooth address.
- **Two families.** The **A24** (CO2-powered rat and stoat trap) sleeps, advertises in a burst when it fires, and uses Goodnature's original GATT services. The rechargeable **Mouse Trap** ("C20" in the protocol notes) advertises continuously and talks over a Nordic UART service. The **Model** sensor shows which a slot holds.
- **Passive, then active.** Strikes are picked up from advertisements without connecting. On a schedule (default 15 min), after a strike, or on a button press, the gateway connects over GATT to sync the trap's clock and read counters, battery and identity. An A24 can only be reached while awake, so its polls happen when it is heard.
- **Consumables.** Lure life and CO2 shots are not available over BLE, so the gateway keeps its own timers, reset with the **Lure Replaced** and **CO2 Canister Replaced** buttons.

## Requirements

- **ESP32-C6 or ESP32-S3** board. `settings.yaml` has a ready-made block for each; the default is `esp32-c6-devkitc-1`. Both are compiled and sized here. Other BLE-capable ESP32s should work by setting `board`, `variant`, `flash_size` and `partitions` to match.
- **ESPHome 2025.12.0 or newer.** The YAML sets `min_version: 2025.12.0` and older versions refuse to build. Developed against 2026.8.
- **Home Assistant 2025.6 or newer** with the ESPHome integration, so sub-devices appear as separate devices.

## Getting started

### 1. Install ESPHome

```bash
python3 -m venv esphome-venv
source esphome-venv/bin/activate
pip install esphome
esphome version   # 2025.12.0 or newer
```

### 2. Configure

```bash
cp settings-example.yaml settings.yaml   # tunables: name, board, slots, poll interval, lure life, CO2 size, ports
cp secrets-example.yaml secrets.yaml     # Wi-Fi, API key, OTA password, fallback hotspot
```

`goodnature-gateway.yaml` is the complete configuration and needs no editing. Both files above are git-ignored. To build the component from git instead of the local checkout, swap the `external_components` source for the commented-out `github://` line.

### 3. Flash

Set `upload_port` in `settings.yaml` (for example `/dev/cu.usbmodem1101` on macOS or `/dev/ttyACM0` on Linux), then:

```bash
make flash          # compile and flash over USB
make logs-serial    # follow the log over USB
make ota            # later: update over the air to ota_address
make logs           # follow the log over the network
```

The Makefile only adds `--device` to `esphome run`, so calling ESPHome directly works too. `upload_speed` and `ota_address` (default `<device_name>.local`; use the IP if mDNS is unreliable) also live in `settings.yaml`.

**Board choice.** The hardware block in `settings.yaml` sets `board`, `variant`, `flash_size` and `partitions` together; uncomment the block for your board and comment out the other. The four values must agree, so check your board's real flash size.

| Board | `variant` | `flash_size` | `partitions` |
|---|---|---|---|
| ESP32-C6 DevKitC-1 (default) | `esp32c6` | `4MB` | `partitions.csv` |
| ESP32-S3, 4 MB | `esp32s3` | `4MB` | `partitions.csv` |
| ESP32-S3-DevKitC-1-N8, 8 MB | `esp32s3` | `8MB` | `partitions-8mb.csv` |

The project ships its own partition tables because the firmware is large: `partitions.csv` gives 1.94 MB app slots on 4 MB flash, `partitions-8mb.csv` gives 3.94 MB on 8 MB. Partition tables only change on a USB flash, and the first flash with one moves the settings area, so the gateway forgets its traps once.

### 4. Add to Home Assistant

Accept the discovered gateway under **Settings → Devices & Services** and enter the API encryption key from `secrets.yaml`. You get one gateway device plus one device per slot in use, named **Goodnature Trap 1 … N**. A fresh gateway shows a single empty slot; each newly discovered trap takes the next slot, and the gateway restarts a few seconds later so Home Assistant gains the device.

![Home Assistant ESPHome integration page listing the Goodnature Gateway device with two Goodnature Trap sub-devices](images/screenshot-1.png)

### 5. Deploy

Power the gateway near the traps. When one advertises, the log shows

```
[I][goodnature_ble] Discovered Goodnature A24 at C0:FF:EE:12:34:56 (RSSI -71), assigned to slot 1
```

and the slot fills in. Rename the device in Home Assistant; the name is kept. A trap keeps its slot until you press **Forget Trap**. A sleeping A24 is not seen until it fires or is woken (shake it, or open the app nearby).

## Configuration

Every option is a substitution in `settings.yaml`, passed through to the `goodnature_ble:` block in the gateway YAML.

| Option | Default | Effect |
|---|---|---|
| `max_traps` | `8` | Number of slots compiled in (~28 entities each). Only slots holding a trap are shown in Home Assistant. |
| `slot_name_prefix` | `Goodnature Trap` | Device names are `<prefix> <n>`. |
| `poll_interval` | `15min` | Minimum time between GATT polls of a trap. `0s` disables scheduled polling. |
| `lure_life_days` | `180` | Default lure life; each trap has a **Lure Life** number to override it. |
| `co2_capacity` | `24` | Shots per CO2 canister (A24). |
| `co2_low_threshold` | `4` | **CO2 Low** turns on at or below this many shots. |
| `offline_timeout_a24` | `24h` | **Online** turns off when an A24 has not been heard for this long. |
| `offline_timeout_c20` | `15min` | Same for the Mouse Trap. |
| `write_time_on_connect` | `true` | Send the current time to the trap on each connection, as the app does. |
| `discovery` | `true` | Initial state of the **Trap Discovery** switch. |
| `auto_acknowledge` | `false` | Queue **Clear Kill Alert** automatically after a poll finds the alert raised. Home Assistant still sees the alert and **Strike** event first. |
| `a24_battery_empty_raw`, `a24_battery_full_raw` | `0`, `0` | Raw A24 battery readings for flat and fresh batteries, once known. `0/0` disables the percentage. |
| `a24_battery_low_percent` | `15` | **Battery Low** threshold for the calibrated A24 percentage. |

Set only in the gateway YAML: `time_id` (a `time:` component, needed for lure timers, timestamps and clock sync) and `hub_entities` (create the gateway-level entities). `settings.yaml` also carries `device_name`, `friendly_name`, `device_description`, `device_area`, `board`, `flash_size`, `upload_port`, `upload_speed`, `ota_address`, `log_level` and `ble_max_connections`.

The component needs `esp32_ble_tracker` and reserves one connection slot in `esp32_ble`. `bluetooth_proxy` can run alongside if you leave it a slot.

## Entities

### Per trap

Each slot in use is a sub-device. A free slot carries the full entity set; once a trap is bound, the next restart registers only the entities for its model. The gateway restarts itself a few seconds after a slot's model changes or a slot comes into or out of use.

Lure and CO2 timers start when a trap is discovered, on the assumption it was just serviced. Press the "Replaced" buttons if not.

![Home Assistant device page for Goodnature Trap 2, a Mouse Trap, showing battery, kill alert, strikes, lure sensors, the Strike event and the configuration buttons](images/screenshot-3.png)

#### Both models

| Entity | Type | Notes |
|---|---|---|
| **Strikes** | sensor, total increasing | A24: `D20D` counter; until first read, inferred from advertisement bursts. Mouse Trap: kill events in the trap's striker log, re-read each poll; test fires not counted. |
| **Kill Alert** | binary, occupancy | A24: unacknowledged kills. Mouse Trap: kill state from the poll; the advertisement's unverified "strikes available" bit can raise it but never clears it. |
| **Strike** | event | `strike` when the counter increases, `test_fire` on a Mouse Trap manual fire. Use this for automations. |
| **Last Strike** | sensor, timestamp | A24: from the strike record, else when the counter was seen to increase. Mouse Trap: the trap's own timestamp on its last kill. |
| **Battery** | sensor, % | A24: only with the `a24_battery_*_raw` calibration. Mouse Trap: from each poll; kept across restarts. |
| **Battery Low** | binary, battery | A24: calibrated percentage at or below `a24_battery_low_percent`. Mouse Trap: LOW, CRITICAL or NOT_CONNECTED from advertisement and poll. |
| **Lure Age**, **Lure Remaining** | sensor, days | From **Lure Replaced** and **Lure Life**. |
| **Lure Due** | binary, problem | On when Lure Remaining ≤ 0. |
| **Online** | binary, connectivity | Heard within the model's offline timeout. |
| **Signal Strength**, **Last Seen** | sensor, diagnostic | Republished at most every 30 s. |
| **Model**, **Serial Number**, **Firmware**, **MAC Address**, **Status** | text, diagnostic | Status: `Unassigned`, `Discovered`, `Waiting for trap`, `Connecting`, `OK`, `Test fire sent`, `Alert cleared`, `Failed: …`. |
| **Last Advertisement**, **Last Frame** | text, diagnostic, disabled | Raw hex captures, filled only while **Debug Mode** is on. |
| **Lure Life** | number, days | Per-trap override of `lure_life_days`. |
| **Poll Now** | button | Connect and read everything. On a Mouse Trap also replays the whole striker log (about 12 s). |
| **Lure Replaced** | button | Restarts the lure timer. |
| **Clear Kill Alert** | button | A24: acknowledge kill pointers and write control `0x02`. Mouse Trap: `SetCommand(CLEAR)`. |
| **Test Fire** | button | **Fires the trap.** A24: control `0x05`. Mouse Trap: `SetCommand(FIRE)`. |
| **Forget Trap** | button | Unbind the slot so it can take a new trap. |
| **Log Debug Info** | button | Dump the slot's internal state to the log at INFO. |

#### A24 only

| Entity | Type | Notes |
|---|---|---|
| **Battery Voltage Raw** | sensor, diagnostic, disabled | Raw `FAD1` value; units unknown. Read it on a fresh and a flat battery to fill in the calibration options. |
| **CO2 Shots Remaining** | sensor | `co2_capacity` minus strikes since **CO2 Canister Replaced**. |
| **CO2 Low** | binary, problem | On at or below `co2_low_threshold`. |
| **CO2 Canister Replaced** | button | Re-anchors the shot counter. |

#### Mouse Trap only

| Entity | Type | Notes |
|---|---|---|
| **Charging** | binary, diagnostic | Charging state from the poll; kept across restarts. |
| **Armed** | binary, diagnostic | Activated state from the poll; kept across restarts. |

### Gateway

![Home Assistant device page for the Goodnature Gateway showing Traps Discovered, the Debug Mode and Trap Discovery switches, Forget All Traps, and the diagnostic entities](images/screenshot-2.png)

| Entity | Notes |
|---|---|
| **Traps Discovered** | Slots in use; the gateway registers this many trap devices (at least one). |
| **Last Discovered Trap** | MAC of the most recently bound trap. |
| **Trap Discovery** | Switch. Turn off once your traps are bound so a neighbour's trap cannot take a slot. Restored across reboots. |
| **Debug Mode** | Switch, off by default. Logs every Goodnature-like advertisement with raw bytes at INFO (one per device per 10 s), dumps the GATT table on every connection, and fills the raw capture text sensors. |
| **Forget All Traps** | Button. Unbinds every slot. |
| **Gateway Version** | Text, diagnostic. Release version of this firmware, also shown as the device's firmware version. |

Plus the usual Wi-Fi, uptime, free heap, ESPHome version and restart entities.

## Debugging and protocol work

`tools/gateway_inspect.py` talks to the gateway over the native API, so no serial cable is needed:

```bash
make states                      # every entity, grouped by device
make capture                     # Debug Mode on for 60 s: decoded advertisements, GATT tables, UART frames
make capture SECONDS=180 POLL=1  # longer, and press Poll Now on every slot first
tools/gateway_inspect.py decode "UART 10/01:e46cb26a01000104000000000000"   # decode a pasted value offline
tools/gateway_inspect.py press "Test Fire" --device "Goodnature Trap 1"      # any button, by name
```

`capture` prints each raw advertisement and frame with a decoded `->` line, using the same layouts as `protocol.cpp`. Bytes not understood yet (5 and 8 of the Mouse Trap advertisement) are shown raw, and the gateway logs `advertisement bytes changed` whenever they move. Keep the decoders in the tool and in `protocol.cpp` in step.

**Verifying kill detection.** With an armed Mouse Trap in range, run `make capture SECONDS=300`, trip the trap with a stick (never a finger), and expect: `advertisement bytes changed`, `polling now to see what changed`, then a poll showing `kill DETECTED` and a `TRIGGER` striker event. In Home Assistant, **Strikes** goes up by one, **Strike** fires, and **Kill Alert** turns on until **Clear Kill Alert** is pressed. If the advertisement did not change, the kill is noticed at the next scheduled poll. Please attach the capture to an issue either way.

**Unrecognised trap or odd values.** Turn on **Debug Mode** and follow the log. Every Goodnature-like advertisement is logged as `ADV <model|unclassified> [MAC] … raw adv=… scan_rsp=…`; `unclassified` means the classifier needs the UUIDs from that line. On connection the full GATT table is printed; compare it with [docs/ble-protocol.md](docs/ble-protocol.md). Enable **Last Advertisement** and **Last Frame** to capture bytes from Home Assistant history, and press **Log Debug Info** to dump a slot's state. Turn Debug Mode off afterwards; it is noisy with a Mouse Trap nearby.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Nothing discovered | Trap asleep (A24) or out of range. Wake it, check RSSI at `DEBUG`, and make sure **Trap Discovery** is on. |
| `all N slots are in use` in the log | More traps in range than `max_traps`. Raise it and reflash, or turn discovery off and forget unwanted slots. |
| `Failed: connect failed` on an A24 | The trap went back to sleep first. Normal occasionally; the gateway backs off (1, 2, 4 … 30 min) and retries on the next advertisement. |
| `Failed: timeout` | A GATT session took longer than 45 s. Usually range. |
| Strikes jumps after the first poll | Expected on the A24: inferred burst counts are replaced by the trap's own counter. |
| Mouse Trap **Battery** is stale | It only updates on a poll. Press **Poll Now** or shorten `poll_interval`. A small single-digit reading is the advertisement's battery *state* leaking through; report it with a capture. |
| Lure sensors `unknown` | No time source yet. Check `time:` and that SNTP can reach the internet, or point it at Home Assistant. |
| Old values after **Forget Trap** | Sensors are cleared, but Home Assistant keeps history. |

For a capture to attach to an issue, set `logger.level: DEBUG` and `logs: { goodnature_ble: DEBUG, goodnature_ble.trap: DEBUG, goodnature_ble.conn: DEBUG }`.

## Project layout

| Path | Contents |
|---|---|
| `goodnature-gateway.yaml` | Complete gateway config. |
| `partitions.csv`, `partitions-8mb.csv` | Partition tables for 4 MB and 8 MB boards. |
| `Makefile` | `make flash`, `make ota`, `make logs`, `make test`; reads the port and OTA address from `settings.yaml`. |
| `settings-example.yaml`, `secrets-example.yaml` | Templates for `settings.yaml` and `secrets.yaml`. |
| `components/goodnature_ble/` | The ESPHome external component (Python codegen + C++). |
| `tests/` | Host-side protocol tests, `./tests/run_tests.sh`. |
| `tools/gateway_inspect.py` | API client for `make states` / `make capture`; decodes captures. |
| `docs/ble-protocol.md` | Protocol reference. |

## Development

- `protocol.{h,cpp}` is plain C++ with no ESPHome dependency: UUIDs, advertisement parsing, C20 UART framing (CRC16-CCITT, escaping, reassembly) and payload decoders. `tests/run_tests.sh` runs its tests with the host compiler.
- `goodnature_hub` does discovery and scheduling, `goodnature_trap` holds per-slot state and entities, `goodnature_connection` is the scripted GATT session on ESPHome's `BLEClientBase`.
- Releases: bump `esphome.project.version` in the YAML, commit, and tag `vx.y`. Home Assistant shows it as the firmware version.
- Slot sub-devices are created by codegen but registered with the App by `GoodnatureHub::setup()`, which registers only the slots in use (at least one). Because the App tables cannot change after boot, a slot coming into or out of use schedules a restart. The tables are still sized for `max_traps` via `ESPHOME_DEVICE_COUNT`, and the hub pads unused device-info entries with a duplicate of slot 1, which Home Assistant merges. This leans on ESPHome internals (the platform `setup_<x>_core_()` helpers, `ESPHOME_DEVICE_COUNT` and the fixed-size device list), so check them when bumping `min_version`.

## Limitations and unknowns

- Sub-device names are fixed at compile time; rename them in Home Assistant.
- A24 battery is only a raw value with unknown units until calibrated.
- A24 `D20D` is assumed to be a lifetime strike counter. If it resets on **Clear Kill Alert**, **Strikes** may go backwards; the gateway logs a warning if it does.
- Mouse Trap strikes are read incrementally (events since the last poll, with overlap and deduplication); the full log is replayed daily, on **Poll Now**, and when the count is unknown. Events stamped before 2017 are ignored.
- Mouse Trap advertisement bytes 5 and 8 do not match the reference integration's layout, so armed and charging state come only from the poll. Any change in those bytes triggers an immediate poll (at most once a minute).
- A trap's kill timestamp can differ from what the app showed; **Last Strike** reports the trap's own value.
- C20 UART escaping is documented only for the frame delimiters; this implementation also escapes a literal `0xB2`.
- Bindings, button presses and new strikes are flushed to flash immediately; last-seen times ride the 60 s periodic sync.

## License and acknowledgements

Provided as-is for personal, non-commercial use, with no affiliation with or endorsement by Goodnature Limited. Protocol details come from the [ha-goodnature](https://github.com/codyc1515/ha-goodnature) integration by codyc1515. AI assistance was used for code and documentation.
