#!/usr/bin/env python3
"""Drive tools/esphome_host/host_server.cpp with the real aioesphomeapi client
and python-zeroconf.

  check_client.py api  --port P [--debug] --version V [--hold S]
  check_client.py mdns --udp-port P --tcp-port P

Expected object_ids, keys, names and attributes below are an independent copy
of design.md ("Entities and keys"), so a typo in the C++ table is caught.
Exit code 0 only when every assertion holds.
"""
import argparse
import asyncio
import socket
import sys
import time

from aioesphomeapi import (
    APIClient,
    BinarySensorInfo,
    BinarySensorState,
    EntityCategory,
    SensorInfo,
    SensorState,
    SensorStateClass,
    TextSensorInfo,
    TextSensorState,
)

DIAG = EntityCategory.DIAGNOSTIC
NONE = EntityCategory.NONE

T, B, S = "text", "binary", "sensor"
M, TI, NO = SensorStateClass.MEASUREMENT, SensorStateClass.TOTAL_INCREASING, SensorStateClass.NONE

# object_id, key, kind, name, unit, device_class, state_class, category
BASIC = [
    ("now_playing", 0xF1142A44, T, "Now playing", "", "", None, NONE),
    ("playing", 0x6E6F9C21, B, "Playing", "", "running", None, NONE),
    ("speaker", 0xA3DA01BC, T, "Speaker", "", "", None, NONE),
    ("volume", 0x8FFEBE43, S, "Volume", "%", "", M, NONE),
    ("wifi_rssi", 0x3800D7CC, S, "WiFi signal", "dBm", "signal_strength", M, DIAG),
    ("uptime", 0x4EEE9F69, S, "Uptime", "s", "duration", TI, DIAG),
]
DEBUG = [
    ("heap_free", 0x86134E68, S, "Heap free", "kB", "", M, DIAG),
    ("heap_min", 0x96739752, S, "Heap minimum", "kB", "", M, DIAG),
    ("heap_largest", 0xB038BE4E, S, "Heap largest block", "kB", "", M, DIAG),
    ("dma_free", 0x1359363A, S, "DMA free", "kB", "", M, DIAG),
    ("psram_free", 0x18DB5D77, S, "PSRAM free", "kB", "", M, DIAG),
    ("stack_main", 0x894B77F1, S, "Stack main", "B", "", M, DIAG),
    ("stack_art", 0x076647AD, S, "Stack art", "B", "", M, DIAG),
    ("stack_net", 0x0855003F, S, "Stack network", "B", "", M, DIAG),
    ("stack_poll", 0xBFEB5CC1, S, "Stack poll", "B", "", M, DIAG),
    ("stack_clkbg", 0xE98B50BF, S, "Stack clock photo", "B", "", M, DIAG),
    ("stack_lyrics", 0x018BC57A, S, "Stack lyrics", "B", "", M, DIAG),
    ("reset_reason", 0xB08FB6BD, T, "Reset reason", "", "", None, DIAG),
    ("reboot_history", 0x4A8ECB93, T, "Reboot history", "", "", None, DIAG),
    ("unexpected_reboots", 0x609749A3, S, "Unexpected reboots", "", "", M, DIAG),
    ("wifi_reconnects", 0x4DB38DFF, S, "WiFi reconnects", "", "", TI, DIAG),
    ("discovery_runs", 0x35904D76, S, "Discovery runs", "", "", TI, DIAG),
    ("art_dma_aborts", 0x7C3DE15B, S, "Art DMA aborts", "", "", TI, DIAG),
    ("dma_recoveries", 0xE3632BE5, S, "DMA recoveries", "", "", TI, DIAG),
    ("ha_shed_events", 0xDAB3288F, S, "HA shed events", "", "", TI, DIAG),
    ("ha_tx_deferrals", 0x2A82613A, S, "HA send deferrals", "", "", TI, DIAG),
    ("speaker_count", 0x084E8318, S, "Speaker count", "", "", M, DIAG),
    ("sync_refresh_age", 0x777E5F70, S, "Speaker refresh age", "s", "duration", M, DIAG),
    ("sync_daily_age", 0xDF43F5C6, S, "Daily check age", "s", "duration", M, DIAG),
    ("coredump_present", 0x006F984A, B, "Core dump present", "", "problem", None, DIAG),
]

INFO_TYPE = {T: TextSensorInfo, B: BinarySensorInfo, S: SensorInfo}
STATE_TYPE = {T: TextSensorState, B: BinarySensorState, S: SensorState}
NAME = "sonosesp-123456"  # last 3 MAC bytes of the harness MAC 24:EC:4A:12:34:56
MAC = "24:EC:4A:12:34:56"

failures = []


def check(cond, msg):
    if cond:
        print(f"  ok   {msg}")
    else:
        print(f"  FAIL {msg}")
        failures.append(msg)


async def wait_for(pred, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        await asyncio.sleep(0.1)
    return pred()


async def run_api(args):
    expected = BASIC + (DEBUG if args.debug else [])
    mode = "debug" if args.debug else "no-debug"
    print(f"[api/{mode}] connecting to 127.0.0.1:{args.port}")
    client = APIClient("127.0.0.1", args.port, None, noise_psk=None, expected_name=NAME)
    lost = []

    async def on_stop(expected_disconnect):
        lost.append(expected_disconnect)

    await asyncio.wait_for(client.connect(on_stop=on_stop, login=True), 10)
    check(True, "connect + hello + authentication (no password, no noise_psk)")

    info = await asyncio.wait_for(client.device_info(), 10)
    check(info.name == NAME, f"device_info.name == {NAME} (got {info.name!r})")
    check(info.mac_address == MAC, f"mac_address == {MAC} (got {info.mac_address!r})")
    check(
        info.esphome_version == args.version,
        f"esphome_version == FIRMWARE_VERSION {args.version} (got {info.esphome_version!r})",
    )
    check(info.model == "JC4880P443C 4in (ESP32-P4 v1.3)", f"model (got {info.model!r})")
    check(info.manufacturer == "SonosESP", f"manufacturer (got {info.manufacturer!r})")
    check(info.project_name == "OpenSurface.SonosESP", f"project_name (got {info.project_name!r})")
    check(info.project_version == args.version, f"project_version (got {info.project_version!r})")
    check(info.friendly_name == "SonosESP Panel", f"friendly_name (got {info.friendly_name!r})")
    check(not info.uses_password, "uses_password is false")
    check(bool(info.compilation_time), f"compilation_time present ({info.compilation_time!r})")
    check(
        not getattr(info, "api_encryption_supported", False),
        "api_encryption_supported is false",
    )

    entities, services = await asyncio.wait_for(client.list_entities_services(), 10)
    want = len(expected)
    check(len(entities) == want, f"{want} entities listed (got {len(entities)})")
    check(not services, "no services listed")
    keys = [e.key for e in entities]
    check(len(set(keys)) == len(keys), "entity keys are unique")
    by_id = {e.object_id: e for e in entities}
    for oid, key, kind, name, unit, dclass, sclass, cat in expected:
        e = by_id.get(oid)
        if e is None:
            check(False, f"entity {oid} listed")
            continue
        problems = []
        if not isinstance(e, INFO_TYPE[kind]):
            problems.append(f"type {type(e).__name__}")
        if e.key != key:
            problems.append(f"key 0x{e.key:08X} != 0x{key:08X}")
        if e.name != name:
            problems.append(f"name {e.name!r} != {name!r}")
        if e.entity_category != cat:
            problems.append(f"category {e.entity_category} != {cat}")
        if kind in (S, B) and e.device_class != dclass:
            problems.append(f"device_class {e.device_class!r} != {dclass!r}")
        if kind == S:
            if e.unit_of_measurement != unit:
                problems.append(f"unit {e.unit_of_measurement!r} != {unit!r}")
            if e.state_class != sclass:
                problems.append(f"state_class {e.state_class} != {sclass}")
            if e.accuracy_decimals != 0:
                problems.append(f"accuracy {e.accuracy_decimals}")
        check(not problems, f"entity {oid}" + (": " + "; ".join(problems) if problems else ""))
    if not args.debug:
        check(
            not any(e.object_id == "heap_free" for e in entities),
            "debug entities absent without --debug",
        )

    states = {}
    history = {}

    def on_state(s):
        states[s.key] = s
        history.setdefault(s.key, []).append(s)

    client.subscribe_states(on_state)
    got_all = await wait_for(lambda: len(states) >= want, 5)
    check(got_all, f"initial states for all {want} entities within 5 s (got {len(states)})")
    for oid, key, kind, *_ in expected:
        s = states.get(key)
        if s is None:
            continue
        if not isinstance(s, STATE_TYPE[kind]):
            check(False, f"state type for {oid}: {type(s).__name__}")
    np = states.get(0xF1142A44)
    check(
        isinstance(np, TextSensorState) and np.state.startswith("Beyonc\u00e9 - Track "),
        f"now_playing text round-trips UTF-8 (got {getattr(np, 'state', None)!r})",
    )
    spk = states.get(0xA3DA01BC)
    check(getattr(spk, "state", None) == "Living Room", f"speaker == Living Room (got {getattr(spk, 'state', None)!r})")
    vol = states.get(0x8FFEBE43)
    check(isinstance(vol, SensorState) and not vol.missing_state, "volume has a value")
    if args.debug:
        daily = states.get(0xDF43F5C6)
        check(
            isinstance(daily, SensorState) and daily.missing_state,
            "sync_daily_age arrives as missing_state",
        )
        stack = states.get(0x894B77F1)
        check(isinstance(stack, SensorState) and stack.state == 1000.0, f"stack_main == 1000 (got {getattr(stack, 'state', None)})")
        hist = states.get(0x4A8ECB93)
        check(
            isinstance(hist, TextSensorState) and hist.state == "power on 12s; software 3600s",
            "reboot_history text",
        )
        heap = states.get(0x86134E68)
        check(
            isinstance(heap, SensorState) and 195 <= heap.state <= 207,
            f"heap_free reported in kB (got {getattr(heap, 'state', None)})",
        )

    if args.quiet:
        await _quiet_checks(client, lost, args)
        return
    first_vol = len(history.get(0x8FFEBE43, []))
    first_np = len(history.get(0xF1142A44, []))
    updated = await wait_for(
        lambda: len(history.get(0x8FFEBE43, [])) > first_vol
        and len(history.get(0xF1142A44, [])) > first_np,
        8,
    )
    check(updated, "state updates arrive after the initial states (volume and now_playing)")
    if len(history.get(0xF1142A44, [])) > first_np:
        newest = history[0xF1142A44][-1].state
        check(newest != history[0xF1142A44][0].state, f"now_playing changed to {newest!r}")

    hold = args.hold
    print(f"[api/{mode}] holding the connection for {hold} s (keepalive is 20 s)")
    await asyncio.sleep(hold)
    check(not lost, "still connected after the hold (no on_stop)")
    check(len(history.get(0x4EEE9F69, [])) >= 1, "uptime state present")

    await asyncio.wait_for(client.disconnect(), 10)
    check(True, "clean disconnect")
    await asyncio.sleep(0.3)


async def _quiet_checks(client, lost, args):
    """Idle connection: the real client keepalive Ping fires and is answered."""
    print(f"[api/quiet] idle for {args.hold} s, no server pushes; the client pings after about 2 keepalive intervals (40 s)")
    await asyncio.sleep(args.hold)
    check(not lost, "still connected after the keepalive interval (no on_stop)")
    await asyncio.wait_for(client.disconnect(), 10)
    check(True, "clean disconnect")
    await asyncio.sleep(0.5)
    await _raw_ping(args.port)


async def _raw_ping(port):
    """Wire-level proof the Ping is answered: Hello then Ping over a raw socket."""
    print("[api/quiet] raw socket: Hello, then Ping, expect HelloResponse then PingResponse")
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    hello = bytes([0x0A, 0x04]) + b"test" + bytes([0x10, 0x01])  # client_info, api_version_major=1
    writer.write(bytes([0x00, len(hello), 0x01]) + hello)
    writer.write(bytes([0x00, 0x00, 0x07]))
    await writer.drain()
    data = b""
    try:
        while not data.endswith(bytes([0x00, 0x00, 0x08])):
            chunk = await asyncio.wait_for(reader.read(512), 3)
            if not chunk:
                break
            data += chunk
    except asyncio.TimeoutError:
        pass
    check(data.startswith(bytes([0x00]) ) and data[2:3] == bytes([0x02]), "HelloResponse (type 2) first")
    check(data.endswith(bytes([0x00, 0x00, 0x08])), "PingResponse (00 00 08) received")
    writer.close()
    await writer.wait_closed()


def run_mdns(args):
    from zeroconf import (
        DNSAddress,
        DNSIncoming,
        DNSOutgoing,
        DNSPointer,
        DNSQuestion,
        DNSService,
        DNSText,
    )
    from zeroconf import const

    def ask(name, qtype, timeout):
        out = DNSOutgoing(const._FLAGS_QR_QUERY)
        out.add_question(DNSQuestion(name, qtype, const._CLASS_IN))
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(timeout)
        try:
            for pkt in out.packets():
                sock.sendto(pkt, ("127.0.0.1", args.udp_port))
            data, _ = sock.recvfrom(2000)
        except socket.timeout:
            return None
        finally:
            sock.close()
        return DNSIncoming(data)

    print(f"[mdns] query PTR _esphomelib._tcp.local to 127.0.0.1:{args.udp_port}")
    msg = ask("_esphomelib._tcp.local.", const._TYPE_PTR, 3)
    check(msg is not None, "reply received")
    if msg is None:
        return
    check(msg.is_response(), "reply has the response flag")
    recs = msg.answers()
    ptr = [r for r in recs if isinstance(r, DNSPointer)]
    srv = [r for r in recs if isinstance(r, DNSService)]
    txt = [r for r in recs if isinstance(r, DNSText)]
    adr = [r for r in recs if isinstance(r, DNSAddress)]
    inst = f"{NAME}._esphomelib._tcp.local."
    check(len(ptr) == 1 and ptr[0].name == "_esphomelib._tcp.local." and ptr[0].alias == inst, f"PTR -> {inst}")
    check(len(srv) == 1 and srv[0].port == args.tcp_port, f"SRV port == {args.tcp_port}")
    check(len(srv) == 1 and srv[0].server == f"{NAME}.local.", f"SRV target {NAME}.local.")
    mac_hex = MAC.replace(":", "").lower()
    props = {}
    if txt:
        raw = txt[0].text
        i = 0
        while i < len(raw):
            ln = raw[i]
            item = raw[i + 1 : i + 1 + ln].decode()
            k, _, v = item.partition("=")
            props[k] = v
            i += 1 + ln
    check(props.get("mac") == mac_hex and len(mac_hex) == 12, f"TXT mac={mac_hex} (got {props})")
    check(
        len(adr) == 1 and adr[0].address == bytes([127, 0, 0, 1]) and adr[0].name == f"{NAME}.local.",
        "A record for the instance host",
    )

    print("[mdns] query A for the instance host")
    msg = ask(f"{NAME}.local.", const._TYPE_A, 3)
    check(
        msg is not None and any(isinstance(r, DNSAddress) for r in msg.answers()),
        "A query answered",
    )

    print("[mdns] query for another host gets no reply")
    msg = ask("somebody-else.local.", const._TYPE_A, 0.6)
    check(msg is None, "no reply for a name that is not ours")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("api")
    a.add_argument("--port", type=int, required=True)
    a.add_argument("--debug", action="store_true")
    a.add_argument("--version", required=True)
    a.add_argument("--hold", type=float, default=25.0)
    a.add_argument("--quiet", action="store_true")
    m = sub.add_parser("mdns")
    m.add_argument("--udp-port", type=int, required=True)
    m.add_argument("--tcp-port", type=int, required=True)
    args = ap.parse_args()
    if args.cmd == "api":
        asyncio.run(run_api(args))
    else:
        run_mdns(args)
    if failures:
        print(f"RESULT: FAIL ({len(failures)} failed)")
        sys.exit(1)
    print("RESULT: PASS")


if __name__ == "__main__":
    main()
