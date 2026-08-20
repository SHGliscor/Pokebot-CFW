#!/usr/bin/env python3
"""Standalone POKEBOT3DS-CFW input bridge v0p2 hardware probe.

This probe exercises only the experimental acknowledged controller on UDP/4952.
Keep legacy InputRedirection OFF while using it.

The soak test is intentionally automated so one run can identify the exact
sequence where transport/state handling fails instead of requiring many manual
one-off probes. By default it uses L pulses; choose a different button only when
it is safe for the current game screen.
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

    def next_request_id(self) -> int:
        self.transaction = (self.transaction + 1) & 0xFFFFFFFF
        if self.transaction == 0:
            self.transaction = 1
        return self.transaction

    def request(self, command: int, *, request_id: int | None = None,
                argument: int = 0, aux: int = 0):
        rid = self.next_request_id() if request_id is None else request_id & 0xFFFFFFFF
        packet = REQ.pack(REQ_MAGIC, VERSION, command, rid,
                          argument & 0xFFFFFFFF, aux & 0xFFFFFFFF)
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


def unpack_input_status(payload: bytes):
    if len(payload) != INPUT_STATUS.size:
        raise BridgeError(f"input status payload was {len(payload)} bytes, expected {INPUT_STATUS.size}")
    return INPUT_STATUS.unpack(payload)


def print_input_status(payload: bytes):
    sequence, state, raw_hid, remaining_ms, runtime_flags = unpack_input_status(payload)
    print(
        f"sequence={sequence} state={input_state_name(state)} "
        f"rawHID=0x{raw_hid:03X} remaining={remaining_ms}ms "
        f"flags=0x{runtime_flags:08X}"
    )
    return sequence, state, raw_hid, remaining_ms, runtime_flags


def normal_ping(bridge: Bridge) -> None:
    status, result, _, payload = bridge.request(CMD_PING)
    print(f"RAM bridge PING: {status_name(status)} result={result} payload={payload.decode('ascii', 'replace')!r}")


def input_ping(bridge: Bridge):
    status, result, _, payload = bridge.request(CMD_INPUT_PING)
    print(f"INPUT_PING: {status_name(status)} result={result}")
    if status != 0:
        return None
    if len(payload) != CAPS.size:
        raise BridgeError(f"capability payload was {len(payload)} bytes, expected {CAPS.size}")
    protocol, capabilities, runtime, neutral, max_hold, max_settle = CAPS.unpack(payload)
    print(f"protocol={protocol} capabilities=0x{capabilities:08X} runtime_flags=0x{runtime:08X}")
    print(f"neutral_HID=0x{neutral:03X} max_hold={max_hold}ms max_settle={max_settle}ms")
    print("Legacy InputRedirection active:", "YES" if runtime & 0x2 else "NO")
    print("Pokebot HID hook active:", "YES" if runtime & 0x1 else "NO")
    return runtime


def release_all(bridge: Bridge) -> None:
    status, result, _, payload = bridge.request(CMD_RELEASE_ALL)
    print(f"RELEASE_ALL: {status_name(status)} result={result}")
    if payload:
        print_input_status(payload)


def raw_hid_for(button: str) -> int:
    key = button.upper()
    if key not in BUTTON_MASKS:
        raise BridgeError(f"unknown button {button!r}")
    return HID_NEUTRAL & ~BUTTON_MASKS[key]


def poll_completion(bridge: Bridge, sequence: int, timeout: float = 4.0,
                    verbose: bool = True) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(0.04)
        status, result, _, payload = bridge.request(CMD_INPUT_STATUS, argument=sequence)
        if status != 0:
            raise BridgeError(f"INPUT_STATUS {status_name(status)} result={result}")
        _, state, *_ = unpack_input_status(payload)
        if verbose:
            print_input_status(payload)
        if state in (3, 4, 5):
            return state
    raise BridgeError(f"sequence {sequence} did not reach terminal state within {timeout:.1f}s")


def one_pulse(bridge: Bridge, button: str, hold_ms: int, settle_ms: int,
              *, sequence: int | None = None, verbose: bool = True) -> tuple[int, int]:
    if sequence is None:
        sequence = bridge.next_request_id()
    raw_hid = raw_hid_for(button)
    aux = ((settle_ms & 0xFFFF) << 16) | (hold_ms & 0xFFFF)
    status, result, _, payload = bridge.request(
        CMD_INPUT_PULSE, request_id=sequence, argument=raw_hid, aux=aux
    )
    if status != 0:
        raise BridgeError(
            f"sequence {sequence} INPUT_PULSE {status_name(status)} result={result}"
        )
    if verbose:
        print(f"INPUT_PULSE {button.upper()} sequence={sequence}")
        print_input_status(payload)
    terminal = poll_completion(bridge, sequence, verbose=verbose)
    if terminal != 3:
        raise BridgeError(
            f"sequence {sequence} terminal={input_state_name(terminal)}, expected COMPLETED"
        )
    return sequence, terminal


def duplicate_proof(bridge: Bridge, button: str = "A",
                    hold_ms: int = 300, settle_ms: int = 120) -> None:
    sequence = bridge.next_request_id()
    raw_hid = raw_hid_for(button)
    aux = (settle_ms << 16) | hold_ms

    print(f"Sending {button.upper()} sequence={sequence}")
    status, result, _, payload = bridge.request(
        CMD_INPUT_PULSE, request_id=sequence, argument=raw_hid, aux=aux
    )
    print(f"INPUT_PULSE: {status_name(status)} result={result}")
    print_input_status(payload)
    if status != 0:
        return

    time.sleep(0.05)
    print("Retransmitting SAME active sequence; it must not press twice...")
    status, result, _, payload = bridge.request(
        CMD_INPUT_PULSE, request_id=sequence, argument=raw_hid, aux=aux
    )
    print(f"ACTIVE DUPLICATE: {status_name(status)} result={result}")
    print_input_status(payload)

    terminal = poll_completion(bridge, sequence)
    print("Terminal:", input_state_name(terminal))

    print("Retransmitting SAME completed sequence; expect ALREADY_COMPLETED...")
    status, result, _, payload = bridge.request(
        CMD_INPUT_PULSE, request_id=sequence, argument=raw_hid, aux=aux
    )
    print(f"COMPLETED DUPLICATE: {status_name(status)} result={result}")
    _, state, *_ = print_input_status(payload)
    if state != 4:
        raise BridgeError(f"completed duplicate returned {input_state_name(state)}, expected ALREADY_COMPLETED")


def soak(bridge: Bridge, button: str, count: int,
         hold_ms: int, settle_ms: int, interval_ms: int) -> None:
    if count < 1:
        raise BridgeError("count must be >= 1")

    print("POKEBOT INPUT SOAK")
    print(f"button={button.upper()} count={count} hold={hold_ms}ms settle={settle_ms}ms interval={interval_ms}ms")
    print("Legacy InputRedirection MUST remain OFF for the entire test.")

    runtime = input_ping(bridge)
    if runtime is not None and (runtime & 0x2):
        raise BridgeError("legacy InputRedirection is active; aborting soak")

    release_all(bridge)
    started = time.monotonic()

    for index in range(1, count + 1):
        try:
            sequence, _ = one_pulse(
                bridge, button, hold_ms, settle_ms, verbose=False
            )
        except Exception as exc:
            elapsed = time.monotonic() - started
            print(f"FAIL at pulse {index}/{count} after {elapsed:.2f}s: {exc}")
            try:
                input_ping(bridge)
                release_all(bridge)
            except Exception as cleanup_exc:
                print(f"Cleanup/status also failed: {cleanup_exc}")
            raise

        if index == 1 or index % 10 == 0 or index == count:
            elapsed = time.monotonic() - started
            print(f"PASS {index}/{count} sequence={sequence} elapsed={elapsed:.2f}s")

        if interval_ms:
            time.sleep(interval_ms / 1000.0)

    elapsed = time.monotonic() - started
    print(f"SOAK PASS: {count}/{count} pulses completed in {elapsed:.2f}s")
    print("Final bridge/controller state:")
    input_ping(bridge)
    release_all(bridge)
    print("Now run a visible single-A test if you want to confirm the physical HID path still responds after the soak.")


def interactive(host: str, timeout: float) -> int:
    print("POKEBOT3DS-CFW Input Bridge v0p2 Hardware Probe")
    print("--------------------------------------------------")
    print(f"3DS: {host}:{PORT}")
    print("Keep legacy InputRedirection OFF.\n")

    bridge = Bridge(host, timeout)
    try:
        while True:
            print("\n1. RAM bridge PING (safe)")
            print("2. INPUT_PING (safe)")
            print("3. RELEASE_ALL / neutral (safe)")
            print("4. Single A pulse")
            print("5. Duplicate A proof")
            print("6. 100-pulse L soak")
            print("7. 500-pulse L soak")
            print("8. Exit")
            choice = input("Select: ").strip()
            try:
                if choice == "1":
                    normal_ping(bridge)
                elif choice == "2":
                    input_ping(bridge)
                elif choice == "3":
                    release_all(bridge)
                elif choice == "4":
                    one_pulse(bridge, "A", 300, 120)
                elif choice == "5":
                    duplicate_proof(bridge, "A")
                elif choice == "6":
                    soak(bridge, "L", 100, 60, 60, 20)
                elif choice == "7":
                    soak(bridge, "L", 500, 60, 60, 20)
                elif choice == "8":
                    return 0
                else:
                    print("Choose 1-8.")
            except (BridgeError, socket.timeout, OSError) as exc:
                print(f"ERROR: {exc}")
    finally:
        bridge.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default="192.168.0.28", help="3DS LAN IP")
    parser.add_argument("--timeout", type=float, default=1.0, help="UDP response timeout")
    parser.add_argument("--button", default="L", choices=sorted(BUTTON_MASKS), help="button for soak")
    parser.add_argument("--count", type=int, default=100, help="soak pulse count")
    parser.add_argument("--hold-ms", type=int, default=60)
    parser.add_argument("--settle-ms", type=int, default=60)
    parser.add_argument("--interval-ms", type=int, default=20)
    parser.add_argument(
        "command", nargs="?",
        choices=["ping", "input-ping", "release", "a", "duplicate-a", "soak"]
    )
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
            one_pulse(bridge, "A", 300, 120)
        elif args.command == "duplicate-a":
            duplicate_proof(bridge, "A")
        elif args.command == "soak":
            soak(bridge, args.button, args.count,
                 args.hold_ms, args.settle_ms, args.interval_ms)
    except (BridgeError, socket.timeout, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
