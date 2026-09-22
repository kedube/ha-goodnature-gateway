#!/usr/bin/env python3
"""Inspect a running gateway over the ESPHome native API.

Talks to the gateway the same way Home Assistant does, so you can see what it
reports and capture raw BLE data for protocol work without a serial cable.

    tools/gateway_inspect.py states                 # every entity, grouped by device
    tools/gateway_inspect.py capture --seconds 60   # debug mode on, stream raw data
    tools/gateway_inspect.py capture --poll         # ...and connect to each trap first

Connection details come from settings.yaml (ota_address / device_name) and
secrets.yaml (api_encryption_key); override with --host / --key. Needs the
aioesphomeapi package, which ships with ESPHome (`make capture` finds it).
"""

from __future__ import annotations

import argparse
import asyncio
import re
import sys
import time
from pathlib import Path

try:
    from aioesphomeapi import APIClient, LogLevel
except ImportError:  # pragma: no cover
    sys.exit("aioesphomeapi not found; run via `make capture` or `pip install aioesphomeapi`")

ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def _yaml_scalar(path: Path, key: str) -> str | None:
    """Pull `key: "value"` out of a flat YAML file without a YAML parser."""
    if not path.is_file():
        return None
    m = re.search(rf'^\s*{re.escape(key)}:\s*"?([^"\n#]*?)"?\s*(#.*)?$', path.read_text(), re.M)
    return m.group(1).strip() if m else None


def default_host() -> str:
    settings = ROOT / "settings.yaml"
    name = _yaml_scalar(settings, "device_name") or "goodnature-gateway"
    addr = _yaml_scalar(settings, "ota_address") or "${device_name}.local"
    return addr.replace("${device_name}", name)


def default_key() -> str | None:
    return _yaml_scalar(ROOT / "secrets.yaml", "api_encryption_key")


def fmt_state(entity, state) -> str:
    if getattr(state, "missing_state", False):
        return "unknown"
    v = getattr(state, "state", None)
    if isinstance(v, float):
        if v != v:  # NaN
            return "unknown"
        return f"{v:g}"
    return str(v)

# ---------------------------------------------------------------------------
# Decoders for the raw captures, so a capture can be read without the docs.
# Mirror components/goodnature_ble/protocol.{h,cpp}; update both together.
# ---------------------------------------------------------------------------

C20_DEVICE_STATE = ("DEACTIVATED", "ACTIVATED", "ERROR", "NO_DATA")
C20_KILL_STATE = ("CLEARED", "DETECTED", "NO_DATA")
C20_TRAY_STATE = ("OPEN", "CLOSED", "UNKNOWN", "NO_DATA")
C20_BATTERY_STATE = ("STARTUP", "NOT_CONNECTED", "CRITICAL", "LOW", "NORMAL", "NO_DATA")
C20_CHARGE_STATE = ("NOT_CHARGING", "CHARGING", "NO_DATA")
C20_USB_STATE = ("DISCONNECTED", "CONNECTED", "NO_DATA")
C20_STRIKER_SOURCE = ("TRIGGER", "UNUSED", "USER", "NO_DATA")
C20_ADV_FLAG_BITS = {0x01: "activated?", 0x02: "strikes_available?", 0x08: "low_battery?",
                     0x10: "charging?", 0x20: "usb?", 0x40: "discoverable?"}
NORDIC_MFG_RE = re.compile(r"(?:mfg=\[0x0059:|ff5900)([0-9a-fA-F]{18})")


def _enum(values, i: int) -> str:
    return values[i] if i < len(values) else f"?{i}"


def _u32(b: bytes, o: int) -> int:
    return int.from_bytes(b[o:o + 4], "little")


def decode_c20_adv(payload: bytes) -> str:
    """9-byte Nordic manufacturer payload. Bytes 5 and 8 are only partly understood."""
    if len(payload) != 9:
        return f"unexpected length {len(payload)}"
    serial = int.from_bytes(payload[0:4], "little")
    flags = payload[5]
    bits = [name for bit, name in C20_ADV_FLAG_BITS.items() if flags & bit] or ["none"]
    strikes = int.from_bytes(payload[6:8], "little")
    return (f"serial={serial:08X} type={payload[4]} byte5=0x{flags:02X} [{' '.join(bits)}] "
            f"strikes={strikes} byte8={payload[8]} (battery state {_enum(C20_BATTERY_STATE, payload[8])})")


def c20_unframe(raw: bytes) -> tuple[int, int, bytes] | None:
    """B0 <escaped body> B1 -> (type, subtype, payload). B2 xx escapes xx ^ 0x04."""
    if len(raw) < 6 or raw[0] != 0xB0 or raw[-1] != 0xB1:
        return None
    body = bytearray()
    i = 1
    while i < len(raw) - 1:
        b = raw[i]
        if b == 0xB2 and i + 1 < len(raw) - 1:
            i += 1
            b = raw[i] ^ 0x04
        body.append(b)
        i += 1
    if len(body) < 4:
        return None
    return body[0], body[1], bytes(body[2:-2])


def decode_c20_frame(ftype: int, payload: bytes) -> str | None:
    p = payload
    if ftype == 0x45:
        inner = c20_unframe(p)
        if inner is None:
            return "log event (undecodable nested frame)"
        t, st, ip = inner
        return f"log event -> type 0x{t:02X}/{st:02X}: " + (decode_c20_frame(t, ip) or ip.hex())
    if ftype == 0x08:
        return "firmware " + p.split(b"\0", 1)[0].decode(errors="replace")
    if ftype == 0x11 and len(p) >= 5:
        return f"battery level {p[4]}% (device time {_u32(p, 0)})"
    if ftype == 0x10 and len(p) >= 14:
        return (f"device state: {_enum(C20_DEVICE_STATE, p[4])} kill={_enum(C20_KILL_STATE, p[5])} "
                f"tray={_enum(C20_TRAY_STATE, p[6])} battery={_enum(C20_BATTERY_STATE, p[7])} "
                f"charge={_enum(C20_CHARGE_STATE, p[8])} usb={_enum(C20_USB_STATE, p[9])} "
                f"strike_count={_u32(p, 10)} (device time {_u32(p, 0)})")
    if ftype == 0x31 and len(p) >= 21:
        return (f"striker event: count={_u32(p, 4)} source={_enum(C20_STRIKER_SOURCE, p[8])} "
                f"trigger={_u32(p, 9)} fire_ms={_u32(p, 13)} rewind_ms={_u32(p, 17)} at {_u32(p, 0)}")
    if ftype == 0x40 and len(p) >= 12:
        return f"logs response: start={_u32(p, 0)} end={_u32(p, 4)} flags/count={_u32(p, 8)}"
    if ftype == 0x04:
        return f"set time ack (device time {_u32(p, 0)})" if len(p) >= 4 else "set time ack"
    return None


FRAME_RE = re.compile(r"^UART ([0-9a-fA-F]{2})/([0-9a-fA-F]{2}):([0-9a-fA-F]*)$")
A24_READ_RE = re.compile(r"^([0-9A-Fa-f]{4}):([0-9a-fA-F]*)$")


def decode_capture(value: str) -> str | None:
    """Decode a Last Frame / Last Advertisement value or an ADV log line."""
    m = FRAME_RE.match(value)
    if m:
        return decode_c20_frame(int(m.group(1), 16), bytes.fromhex(m.group(3)))
    m = NORDIC_MFG_RE.search(value)
    if m:
        return decode_c20_adv(bytes.fromhex(m.group(1)))
    m = A24_READ_RE.match(value)
    if m:
        raw = bytes.fromhex(m.group(2))
        if len(raw) == 2:
            return f"u16 {int.from_bytes(raw, 'little')}"
        try:
            return "text " + repr(raw.rstrip(b"\0").decode())
        except UnicodeDecodeError:
            return None
    return None



class Gateway:
    def __init__(self, host: str, key: str | None):
        self.client = APIClient(host, 6053, None, noise_psk=key or None, client_info="goodnature-inspect")
        self.entities = []
        self.by_key = {}
        self.device_name = {0: "Gateway"}

    async def __aenter__(self):
        await self.client.connect(login=True)
        info = await self.client.device_info()
        self.info = info
        for d in info.devices:
            self.device_name.setdefault(d.device_id, d.name)
        self.entities, _ = await self.client.list_entities_services()
        # ESPHome entity keys are hashes of the object ID, so "Strikes" has the
        # same key on every trap; only (device_id, key) is unique.
        self.by_key = {(e.device_id, e.key): e for e in self.entities}
        return self

    async def __aexit__(self, *exc):
        await self.client.disconnect()

    def find(self, name: str, device: str | None = None):
        return [
            e
            for e in self.entities
            if e.name == name and (device is None or self.device_name.get(e.device_id) == device)
        ]

    def label(self, entity) -> str:
        return f"{self.device_name.get(entity.device_id, '?')} / {entity.name}"


async def collect_states(gw: Gateway, seconds: float = 2.0) -> dict:
    states = {}

    def on_state(s):
        ident = (getattr(s, "device_id", 0), s.key)
        e = gw.by_key.get(ident)
        if e is not None:
            states[ident] = fmt_state(e, s)

    gw.client.subscribe_states(on_state)
    await asyncio.sleep(seconds)
    return states


async def cmd_states(gw: Gateway, args) -> None:
    states = await collect_states(gw)
    print(f"{gw.info.friendly_name} ({gw.info.mac_address}) ESPHome {gw.info.esphome_version}, "
          f"compiled {gw.info.compilation_time}")
    print("sub-devices:", ", ".join(f"{d.name}#{d.device_id}" for d in gw.info.devices if d.name) or "none")
    rows = sorted(gw.entities, key=lambda e: (gw.device_name.get(e.device_id, "~"), type(e).__name__, e.name))
    current = None
    for e in rows:
        dev = gw.device_name.get(e.device_id, "?")
        if dev != current:
            current = dev
            print(f"\n[{dev}]")
        if type(e).__name__.startswith("Button"):
            continue
        print(f"  {e.name:28s} {states.get((e.device_id, e.key), 'unknown')}")


async def cmd_capture(gw: Gateway, args) -> None:
    t0 = time.monotonic()

    def stamp() -> str:
        return f"{time.monotonic() - t0:7.2f}s"

    watch = {(e.device_id, e.key): e for e in gw.entities if e.name in ("Last Advertisement", "Last Frame", "Status", "Strikes",
                                                          "Battery", "Battery Voltage Raw", "Model", "Firmware",
                                                          "Serial Number")}
    last = {}

    def on_state(s):
        ident = (getattr(s, "device_id", 0), s.key)
        e = watch.get(ident)
        if e is None:
            return
        v = fmt_state(e, s)
        raw_capture = e.name in ("Last Advertisement", "Last Frame")
        if last.get(ident) == v and not raw_capture:
            return  # ordinary sensors: only print changes
        repeat = " (repeat)" if last.get(ident) == v else ""
        last[ident] = v
        print(f"{stamp()} STATE {gw.label(e)}: {v}{repeat}", flush=True)
        if e.name in ("Last Advertisement", "Last Frame") and (d := decode_capture(v)):
            print(f"{'':8s}       -> {d}", flush=True)

    def on_log(msg):
        line = msg.message.decode(errors="replace") if isinstance(msg.message, bytes) else str(msg.message)
        line = ANSI.sub("", line)
        if "goodnature" in line or args.all_logs:
            print(f"{stamp()} LOG   {line.strip()}", flush=True)
            if "ADV " in line and (d := decode_capture(line)):
                print(f"{'':8s}       -> {d}", flush=True)

    gw.client.subscribe_states(on_state)
    gw.client.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_VERY_VERBOSE)

    debug = gw.find("Debug Mode")
    if debug:
        gw.client.switch_command(debug[0].key, True, device_id=debug[0].device_id)
        print(f"{stamp()} debug mode on", flush=True)
    await asyncio.sleep(1)
    for e in gw.find("Log Debug Info"):
        gw.client.button_command(e.key, device_id=e.device_id)
    if args.poll:
        for e in gw.find("Poll Now"):
            gw.client.button_command(e.key, device_id=e.device_id)
        print(f"{stamp()} poll requested for every slot (A24 traps connect when next heard)", flush=True)
    print(f"{stamp()} capturing for {args.seconds}s; wake the trap now if it sleeps", flush=True)
    try:
        await asyncio.sleep(args.seconds)
    finally:
        if debug and not args.keep_debug:
            gw.client.switch_command(debug[0].key, False, device_id=debug[0].device_id)
            await asyncio.sleep(0.5)
            print(f"{stamp()} debug mode off", flush=True)


async def cmd_press(gw: Gateway, args) -> None:
    matches = gw.find(args.button, args.device)
    if not matches:
        sys.exit(f"no button named {args.button!r}" + (f" on {args.device!r}" if args.device else ""))
    for e in matches:
        gw.client.button_command(e.key, device_id=e.device_id)
        print("pressed", gw.label(e))
    await asyncio.sleep(0.5)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=default_host())
    p.add_argument("--key", default=default_key(), help="API encryption key (default: secrets.yaml)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("states", help="print every entity state").set_defaults(fn=cmd_states)
    c = sub.add_parser("capture", help="turn on Debug Mode and stream raw advertisements, GATT frames and logs")
    c.add_argument("--seconds", type=float, default=60)
    c.add_argument("--poll", action="store_true", help="press Poll Now on every slot first")
    c.add_argument("--keep-debug", action="store_true", help="leave Debug Mode on afterwards")
    c.add_argument("--all-logs", action="store_true", help="show every log line, not just goodnature_ble")
    c.set_defaults(fn=cmd_capture)
    d = sub.add_parser("decode", help="decode captured values offline, e.g. 'UART 10/01:e46c...' or an ADV log line")
    d.add_argument("values", nargs="+")
    d.set_defaults(fn=None)
    b = sub.add_parser("press", help="press a button, e.g. press 'Poll Now' --device 'Goodnature Trap 1'")
    b.add_argument("button")
    b.add_argument("--device")
    b.set_defaults(fn=cmd_press)
    args = p.parse_args()

    if args.cmd == "decode":
        for v in args.values:
            print(f"{v}\n  -> {decode_capture(v) or 'not recognised'}")
        return

    async def run():
        async with Gateway(args.host, args.key) as gw:
            await args.fn(gw, args)

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
