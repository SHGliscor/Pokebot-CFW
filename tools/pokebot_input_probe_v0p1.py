#!/usr/bin/env python3
"""Standalone Pokebot-CFW input bridge hardware probe.

This probe intentionally does not depend on the Qt bot. It exercises only the
new UDP/4952 controller commands. Start with INPUT_PING and RELEASE_ALL. Only
choose an A-pulse test when you are ready for the 3DS to receive an A press.
"""

from __future__ import annotations

import argparse
import random
import socket
import struct
import sys
import time

REQ_MAGIC = 0x5242524F
RESP_MAGIC = 0x5342524F
VERSION = 1
PORT = 4952

CMD_PING = 1
CMD_INPUT_PING = 5
CMD_INPUT_PULSE = 6
CMD_INPUT_STATUS = 7
CMD_RELEASE_ALL = 8

STATUS_NAMES = {
    0: "OK",
    1: "BAD_MAGIC",
    2: "BAD_VERSION",
    3: "BAD_COMMAND",
    12: "INPUT_INVALID",
    13: "INPUT_BUSY",
    14: "INPUT_LEGACY_ACTIVE",
    15: "INPUT_PATCH_FAILED",
}

INPUT_STATE_NAMES = {
    0: "IDLE",
    1: "ACCEPTED",
    2: "IN_PROGRESS",
    3: "COMPLETED",
    4: "ALREADY_COMPLETED",
    5: "ABORTED",
    6: "NOT_FOUND",
}

HID_NEUTRAL = 0x00000FFF
BUTTON_MASKS = {
    "A": 1 << 0,
    "B": 1 << 1,
    "SELECT": 1 << 2,
    "START": 1 << 3,
    "RIGHT": 1 << 4,
    "LEFT": 1 << 5,
    "UP": 1 << 6,
    "DOWN": 1 << 7,
    "R": 1 << 8,
    "L": 1 << 9,
    "X": 1 << 10,
    "Y": 1 << 11,
}

REQ = struct.Struct("<IHHIII")
RESP = struct.Struct("<IHHIIiI")
CAPS = struct.Struct("<IIIIII")
INPUT_STATUS = struct.Struct("<IIIII")


class BridgeError(RuntimeError):
    pass


class Bridge:
    def __init__(self, host: str, timeout: float = 1.0):
        self.host = host
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.transaction = random.randint(1, 0x7FFFFFFF)

    def close(self) -> None:
        self.sock.close()

    def _next_request_id(self) -> int:
        self.transaction = (self.transaction + 1) & 0xFFFFFFFF
        if self.transaction == 0:
            self.transaction = 1
        return self.transaction

    def request(self, command: int, *, request_id: int | None = None,
                argument: int = 0, aux: int = 0):
        rid = self._next_request_id() if request_id is None else request_id & 0xFFFFFFFF
        packet = REQ.pack(REQ_MAGIC, VERSION, command, rid, argument & 0xFFFFFFFF, aux & 0xFFFFFFFF)
        self.sock.sendto(packet, (self.host, PORT))
        data, _ = self.sock.recvfrom(1024)
        if len(data) < RESP.size:
            raise BridgeError(f"short response: {len(data)} bytes")
        magic, version, status, response_id, response_arg, result, payload_len = RESP.unpack_from(data)
        if magic != RESP_MAGIC:
            raise BridgeError(f"bad response magic 0x{magic:08X}")
        if version != VERSION:
            raise BridgeError(f"bridge version {version}, expected {VERSION}")
        if response_id != rid:
            raise BridgeError(f"request id mismatch: sent {rid}, got {response_id}")
        payload = data[RESP.size:RESP.size + payload_len]
        if len(payload) != payload_len:
            raise BridgeError(f"truncated payload: expected {payload_len}, got {len(payload)}")
        return status, result, response_arg, payload


def status_name(value: int) -> str:
    return STATUS_NAMES.get(value, f"STATUS_{value}")


def input_state_name(value: int) -> str:
    return INPUT_STATE_NAMES.get(value, f"STATE_{value}")


def print_input_status(payload: bytes) -> tuple[int, int]:
    if len(payload) != INPUT_STATUS.size:
        raise BridgeError(f"input status payload was {len(payload)} bytes, expected {INPUT_STATUS.size}")
    sequence, state, raw_hid, remaining_ms, runtime_flags = INPUT_STATUS.unpack(payload)
    print(
        f"sequence={sequence}  state={input_state_name(state)}  "
        f"rawHID=0x{raw_hid:03X}  remaining={remaining_ms}ms  flags=0x{runtime_flags:08X}"
    )
    return sequence, state


def normal_ping(bridge: Bridge) -> None:
    status, result, _, payload = bridge.request(CMD_PING)
    text = payload.decode("ascii", "replace")
    print(f"RAM bridge PING: {status_name(status)} result={result} payload={text!r}")


def input_ping(bridge: Bridge) -> None:
    status, result, _, payload = bridge.request(CMD_INPUT_PING)
    print(f"INPUT_PING: {status_name(status)} result={result}")
    if status != 0:
        return
    if len(payload) != CAPS.size:
        raise BridgeError(f"capability payload was {len(payload)} bytes, expected {CAPS.size}")
    protocol, capabilities, runtime, neutral, max_hold, max_settle = CAPS.unpack(payload)
    print(f"protocol={protocol}")
    print(f"capabilities=0x{capabilities:08X}")
    print(f"runtime_flags=0x{runtime:08X}")
    print(f"neutral_HID=0x{neutral:03X}")
    print(f"max_hold={max_hold}ms  max_settle={max_settle}ms")
    print("Legacy InputRedirection active:", "YES" if runtime & 0x2 else "NO")
    print("Pokebot HID hook active:", "YES" if runtime & 0x1 else "NO")


def release_all(bridge: Bridge) -> None:
    status, result, _, payload = bridge.request(CMD_RELEASE_ALL)
    print(f"RELEASE_ALL: {status_name(status)} result={result}")
    if payload:
        print_input_status(payload)


def raw_hid_for(buttons: list[str]) -> int:
    pressed = 0
    for button in buttons:
        try:
            pressed |= BUTTON_MASKS[button.upper()]
        except KeyError as exc:
            raise BridgeError(f"unknown button {button!r}") from exc
    return HID_NEUTRAL & ~pressed


def poll_completion(bridge: Bridge, sequence: int, timeout: float = 4.0) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(0.05)
        status, result, _, payload = bridge.request(CMD_INPUT_STATUS, argument=sequence)
        if status != 0:
            print(f"INPUT_STATUS: {status_name(status)} result={result}")
            return -1
        _, state = print_input_status(payload)
        if state in (3, 4, 5):
            return state
    raise BridgeError(f"sequence {sequence} did not reach a terminal state within {timeout:.1f}s")


def pulse(bridge: Bridge, buttons: list[str], *, hold_ms: int = 300,
          settle_ms: int = 120, duplicate: bool = False) -> None:
    sequence = random.randint(1, 0xFFFFFFFF)
    raw_hid = raw_hid_for(buttons)
    aux = (settle_ms << 16) | hold_ms
    label = "+".join(buttons)
    print(f"Sending {label} sequence={sequence} rawHID=0x{raw_hid:03X} hold={hold_ms}ms settle={settle_ms}ms")

    status, result, _, payload = bridge.request(
        CMD_INPUT_PULSE, request_id=sequence, argument=raw_hid, aux=aux
    )
    print(f"INPUT_PULSE: {status_name(status)} result={result}")
    if payload:
        print_input_status(payload)
    if status != 0:
        return

    if duplicate:
        time.sleep(0.05)
        print("Retransmitting the exact same sequence ID. It must NOT press twice...")
        status2, result2, _, payload2 = bridge.request(
            CMD_INPUT_PULSE, request_id=sequence, argument=raw_hid, aux=aux
        )
        print(f"DUPLICATE INPUT_PULSE: {status_name(status2)} result={result2}")
        if payload2:
            print_input_status(payload2)

    terminal = poll_completion(bridge, sequence)
    print(f"Terminal state: {input_state_name(terminal)}")

    print("Re-sending the same completed sequence ID. It must return ALREADY_COMPLETED without another press...")
    status3, result3, _, payload3 = bridge.request(
        CMD_INPUT_PULSE, request_id=sequence, argument=raw_hid, aux=aux
    )
    print(f"COMPLETED DUPLICATE: {status_name(status3)} result={result3}")
    if payload3:
        print_input_status(payload3)


def interactive(host: str, timeout: float) -> int:
    print("Pokebot-CFW Input Bridge v0p1 Hardware Probe")
    print("------------------------------------------------")
    print(f"3DS: {host}:{PORT}")
    print("Keep Luma InputRedirection OFF for these tests.\n")

    bridge = Bridge(host, timeout)
    try:
        while True:
            print("\n1. RAM bridge PING (safe)")
            print("2. INPUT_PING (safe, no controller action)")
            print("3. RELEASE_ALL (safe neutral command)")
            print("4. Single A pulse (physical A press)")
            print("5. Duplicate A proof (must physically press A once only)")
            print("6. Exit")
            choice = input("Select: ").strip()
            try:
                if choice == "1":
                    normal_ping(bridge)
                elif choice == "2":
                    input_ping(bridge)
                elif choice == "3":
                    release_all(bridge)
                elif choice == "4":
                    pulse(bridge, ["A"])
                elif choice == "5":
                    pulse(bridge, ["A"], duplicate=True)
                elif choice == "6":
                    return 0
                else:
                    print("Choose 1-6.")
            except (BridgeError, socket.timeout, OSError) as exc:
                print(f"ERROR: {exc}")
    finally:
        bridge.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default="192.168.0.28", help="3DS LAN IP")
    parser.add_argument("--timeout", type=float, default=1.0, help="UDP response timeout")
    parser.add_argument("command", nargs="?", choices=["ping", "input-ping", "release", "a", "duplicate-a"])
    args = parser.parse_args()

    if args.command is None:
        return interactive(args.ip, args.timeout)

    bridge = Bridge(args.ip, args.timeout)
    try:
        if args.command == "ping":
            normal_ping(bridge)
        elif args.command == "input-ping":
            input_ping(bridge)
        elif args.command == "release":
            release_all(bridge)
        elif args.command == "a":
            pulse(bridge, ["A"])
        elif args.command == "duplicate-a":
            pulse(bridge, ["A"], duplicate=True)
    except (BridgeError, socket.timeout, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
