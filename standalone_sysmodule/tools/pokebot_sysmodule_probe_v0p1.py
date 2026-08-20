from __future__ import annotations

import argparse
import json
import random
import socket
import struct
import time

REQ_MAGIC = 0x5242524F
RESP_MAGIC = 0x5342524F
VERSION = 1
PORT = 4952

CMD_PING = 1
CMD_GAME_INFO = 2
CMD_QUERY = 3
CMD_READ = 4
CMD_INPUT_PING = 5
CMD_INPUT_PULSE = 6
CMD_INPUT_STATUS = 7
CMD_RELEASE_ALL = 8
CMD_INPUT_ARM = 9
CMD_INPUT_DISARM = 10

REQ = struct.Struct("<IHHIII")
RESP = struct.Struct("<IHHIIiI")
GAME_INFO = struct.Struct("<QI8sI")
INPUT_CAPS = struct.Struct("<IIIIII")
INPUT_STATUS = struct.Struct("<IIIII")

STATE_NAMES = {
    0: "IDLE",
    1: "ACCEPTED",
    2: "IN_PROGRESS",
    3: "COMPLETED",
    4: "ALREADY_COMPLETED",
    5: "ABORTED",
    6: "NOT_FOUND",
}

STATUS_NAMES = {
    0: "OK",
    1: "BAD_MAGIC",
    2: "BAD_VERSION",
    3: "BAD_COMMAND",
    4: "GAME_NOT_FOUND",
    5: "OPEN_FAILED",
    6: "QUERY_FAILED",
    7: "NOT_READABLE",
    8: "RANGE_INVALID",
    9: "LENGTH_INVALID",
    10: "MAP_FAILED",
    11: "INTERNAL",
    12: "INPUT_INVALID",
    13: "INPUT_BUSY",
    14: "INPUT_LEGACY_ACTIVE",
    15: "INPUT_PATCH_FAILED",
    16: "INPUT_NOT_ARMED",
    17: "GAME_NOT_READY",
}

BUTTONS = {
    "A": 0xFFE,
    "B": 0xFFD,
    "SELECT": 0xFFB,
    "START": 0xFF7,
    "RIGHT": 0xFEF,
    "LEFT": 0xFDF,
    "UP": 0xFBF,
    "DOWN": 0xF7F,
    "R": 0xEFF,
    "L": 0xDFF,
    "X": 0xBFF,
    "Y": 0x7FF,
}


class BridgeError(RuntimeError):
    pass


def request(host: str, command: int, argument: int = 0, aux: int = 0,
            request_id: int | None = None, timeout: float = 1.5) -> dict:
    if request_id is None:
        request_id = random.randint(1, 0xFFFFFFFF)
    packet = REQ.pack(REQ_MAGIC, VERSION, command, request_id, argument, aux)

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(packet, (host, PORT))
        try:
            data, remote = s.recvfrom(4096)
        except socket.timeout as exc:
            raise BridgeError(f"no response from {host}:{PORT}") from exc

    if len(data) < RESP.size:
        raise BridgeError(f"short response: {len(data)} bytes")

    magic, version, status, echoed_id, echoed_arg, result, payload_len = RESP.unpack_from(data)
    payload = data[RESP.size:]
    if magic != RESP_MAGIC:
        raise BridgeError(f"bad response magic 0x{magic:08X}")
    if version != VERSION:
        raise BridgeError(f"protocol version {version}, expected {VERSION}")
    if echoed_id != request_id:
        raise BridgeError(f"request id mismatch {echoed_id} != {request_id}")
    if len(payload) != payload_len:
        raise BridgeError(f"payload length mismatch {len(payload)} != {payload_len}")

    return {
        "remote": f"{remote[0]}:{remote[1]}",
        "status": status,
        "status_name": STATUS_NAMES.get(status, f"STATUS_{status}"),
        "result": result,
        "result_hex": f"0x{result & 0xFFFFFFFF:08X}",
        "request_id": echoed_id,
        "argument": echoed_arg,
        "payload": payload,
    }


def parse_input_status(r: dict) -> dict:
    payload = r.pop("payload")
    if len(payload) == INPUT_STATUS.size:
        seq, state, raw_hid, remaining_ms, runtime_flags = INPUT_STATUS.unpack(payload)
        r.update({
            "sequence_id": seq,
            "state": state,
            "state_name": STATE_NAMES.get(state, str(state)),
            "raw_hid": f"0x{raw_hid:03X}",
            "remaining_ms": remaining_ms,
            "runtime_flags": f"0x{runtime_flags:08X}",
            "hid_enabled": bool(runtime_flags & (1 << 0)),
            "pulse_active": bool(runtime_flags & (1 << 2)),
            "armed": bool(runtime_flags & (1 << 3)),
            "game_seen": bool(runtime_flags & (1 << 4)),
        })
    else:
        r["payload_hex"] = payload.hex()
    return r


def ping(host: str, timeout: float) -> dict:
    r = request(host, CMD_PING, timeout=timeout)
    r["pong"] = r.pop("payload").decode("ascii", errors="replace")
    return r


def game_info(host: str, timeout: float) -> dict:
    r = request(host, CMD_GAME_INFO, timeout=timeout)
    payload = r.pop("payload")
    if len(payload) == GAME_INFO.size:
        title_id, pid, name, flags = GAME_INFO.unpack(payload)
        r.update({
            "title_id": f"0x{title_id:016X}",
            "pid": pid,
            "name": name.rstrip(b"\0").decode("ascii", errors="replace"),
            "flags": f"0x{flags:08X}",
        })
    else:
        r["payload_hex"] = payload.hex()
    return r


def input_ping(host: str, timeout: float) -> dict:
    r = request(host, CMD_INPUT_PING, timeout=timeout)
    payload = r.pop("payload")
    if len(payload) == INPUT_CAPS.size:
        protocol, caps, runtime, neutral, max_hold, max_settle = INPUT_CAPS.unpack(payload)
        r.update({
            "input_protocol": protocol,
            "capability_flags": f"0x{caps:08X}",
            "runtime_flags": f"0x{runtime:08X}",
            "neutral_hid": f"0x{neutral:03X}",
            "max_hold_ms": max_hold,
            "max_settle_ms": max_settle,
            "explicit_arm": bool(caps & (1 << 6)),
            "direct_hid": bool(caps & (1 << 7)),
            "hid_enabled": bool(runtime & (1 << 0)),
            "armed": bool(runtime & (1 << 3)),
            "game_seen": bool(runtime & (1 << 4)),
        })
    else:
        r["payload_hex"] = payload.hex()
    return r


def arm(host: str, timeout: float) -> dict:
    return parse_input_status(request(host, CMD_INPUT_ARM, timeout=timeout))


def disarm(host: str, timeout: float) -> dict:
    return parse_input_status(request(host, CMD_INPUT_DISARM, timeout=timeout))


def release_all(host: str, timeout: float) -> dict:
    return parse_input_status(request(host, CMD_RELEASE_ALL, timeout=timeout))


def status(host: str, sequence_id: int, timeout: float) -> dict:
    return parse_input_status(request(host, CMD_INPUT_STATUS, argument=sequence_id, timeout=timeout))


def pulse(host: str, raw_hid: int, hold_ms: int, settle_ms: int,
          timeout: float, sequence_id: int | None = None) -> tuple[int, dict]:
    sequence_id = sequence_id or random.randint(1, 0xFFFFFFFF)
    aux = (hold_ms & 0xFFFF) | ((settle_ms & 0xFFFF) << 16)
    r = request(host, CMD_INPUT_PULSE, argument=raw_hid, aux=aux,
                request_id=sequence_id, timeout=timeout)
    return sequence_id, parse_input_status(r)


def wait_completed(host: str, sequence_id: int, timeout: float, deadline_s: float = 5.0) -> dict:
    deadline = time.monotonic() + deadline_s
    last = None
    while time.monotonic() < deadline:
        last = status(host, sequence_id, timeout)
        if last.get("state") in (3, 4, 5, 6):
            return last
        time.sleep(0.05)
    raise BridgeError(f"sequence {sequence_id} did not complete; last={last}")


def read_memory(host: str, address: int, length: int, timeout: float) -> dict:
    r = request(host, CMD_READ, argument=address, aux=length, timeout=timeout)
    r["payload_hex"] = r.pop("payload").hex()
    return r


def smoke(host: str, timeout: float) -> dict:
    report: dict[str, object] = {"host": host, "port": PORT, "steps": []}
    steps: list[dict] = report["steps"]  # type: ignore[assignment]

    steps.append({"ping": ping(host, timeout)})
    pre = input_ping(host, timeout)
    steps.append({"input_ping_before_game_arm": pre})

    info = game_info(host, timeout)
    steps.append({"game_info": info})
    if info.get("status") != 0:
        report["result"] = "FAIL: game not ready; HID was not armed"
        return report

    armed = arm(host, timeout)
    steps.append({"arm": armed})
    if armed.get("status") != 0 or not armed.get("armed"):
        report["result"] = "FAIL: INPUT_ARM failed"
        return report

    seq, accepted = pulse(host, BUTTONS["A"], 300, 120, timeout)
    steps.append({"a_pulse": accepted})
    completed = wait_completed(host, seq, timeout)
    steps.append({"a_completed": completed})

    duplicate = request(host, CMD_INPUT_PULSE, argument=BUTTONS["A"],
                        aux=(300 | (120 << 16)), request_id=seq, timeout=timeout)
    steps.append({"duplicate_same_sequence": parse_input_status(duplicate)})

    steps.append({"release_all": release_all(host, timeout)})
    steps.append({"disarm": disarm(host, timeout)})
    final = input_ping(host, timeout)
    steps.append({"input_ping_after_disarm": final})

    if final.get("hid_enabled") or final.get("armed"):
        report["result"] = "FAIL: module did not return to dormant HID state"
    else:
        report["result"] = "PASS candidate: arm/pulse/dedupe/disarm protocol completed"
    return report


def main() -> None:
    p = argparse.ArgumentParser(description="Pokebot3DS standalone Luma sysmodule v0p1 probe")
    p.add_argument("host", help="3DS IP address")
    p.add_argument("command", choices=[
        "ping", "info", "input-ping", "arm", "disarm", "release",
        "a", "button", "status", "read", "smoke"
    ])
    p.add_argument("--timeout", type=float, default=1.5)
    p.add_argument("--button", choices=sorted(BUTTONS), default="A")
    p.add_argument("--hold-ms", type=int, default=300)
    p.add_argument("--settle-ms", type=int, default=120)
    p.add_argument("--sequence", type=lambda x: int(x, 0), default=0)
    p.add_argument("--address", type=lambda x: int(x, 0), default=0x08CFB26C)
    p.add_argument("--length", type=int, default=16)
    args = p.parse_args()

    if args.command == "ping":
        out = ping(args.host, args.timeout)
    elif args.command == "info":
        out = game_info(args.host, args.timeout)
    elif args.command == "input-ping":
        out = input_ping(args.host, args.timeout)
    elif args.command == "arm":
        out = arm(args.host, args.timeout)
    elif args.command == "disarm":
        out = disarm(args.host, args.timeout)
    elif args.command == "release":
        out = release_all(args.host, args.timeout)
    elif args.command in ("a", "button"):
        name = "A" if args.command == "a" else args.button
        seq, first = pulse(args.host, BUTTONS[name], args.hold_ms,
                           args.settle_ms, args.timeout,
                           args.sequence or None)
        out = {"sequence_id": seq, "accepted": first,
               "terminal": wait_completed(args.host, seq, args.timeout)}
    elif args.command == "status":
        out = status(args.host, args.sequence, args.timeout)
    elif args.command == "read":
        out = read_memory(args.host, args.address, args.length, args.timeout)
    else:
        out = smoke(args.host, args.timeout)

    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
