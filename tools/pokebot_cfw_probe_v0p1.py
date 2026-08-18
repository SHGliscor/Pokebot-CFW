from __future__ import annotations

import argparse
import json
import socket
import struct
import time

REQ_MAGIC = 0x5242524F  # ORBR
RESP_MAGIC = 0x5342524F  # ORBS
VERSION = 1
PORT = 4952

CMD_PING = 1
CMD_GAME_INFO = 2
CMD_QUERY = 3
CMD_READ = 4

STATUS = {
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
}

REQ = struct.Struct("<IHHIII")
RESP = struct.Struct("<IHHIIiI")
GAME_INFO = struct.Struct("<QI8sI")
QUERY_INFO = struct.Struct("<IIIII")


class BridgeError(RuntimeError):
    pass


def request(host: str, command: int, argument: int = 0, aux: int = 0, timeout: float = 2.0) -> dict:
    request_id = int(time.time_ns() & 0xFFFFFFFF)
    packet = REQ.pack(REQ_MAGIC, VERSION, command, request_id, argument & 0xFFFFFFFF, aux & 0xFFFFFFFF)

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(packet, (host, PORT))
        try:
            data, remote = sock.recvfrom(4096)
        except socket.timeout as exc:
            raise BridgeError(f"no response from {host}:{PORT}: timed out") from exc

    if len(data) < RESP.size:
        raise BridgeError(f"short response: {len(data)} bytes")

    magic, version, status, echoed_id, echoed_arg, result, payload_len = RESP.unpack_from(data)
    payload = data[RESP.size:]

    if magic != RESP_MAGIC:
        raise BridgeError(f"bad response magic 0x{magic:08X}")
    if version != VERSION:
        raise BridgeError(f"protocol version {version}, expected {VERSION}")
    if echoed_id != request_id:
        raise BridgeError("request id mismatch")
    if len(payload) != payload_len:
        raise BridgeError(f"payload length mismatch: header={payload_len} actual={len(payload)}")

    return {
        "remote": f"{remote[0]}:{remote[1]}",
        "status": status,
        "status_name": STATUS.get(status, f"UNKNOWN_{status}"),
        "result": result,
        "result_hex": f"0x{result & 0xFFFFFFFF:08X}",
        "argument": echoed_arg,
        "payload_len": payload_len,
        "payload": payload,
    }


def ping(host: str, timeout: float) -> dict:
    r = request(host, CMD_PING, timeout=timeout)
    r["pong"] = r.pop("payload").decode("ascii", errors="replace")
    return r


def game_info(host: str, timeout: float) -> dict:
    r = request(host, CMD_GAME_INFO, timeout=timeout)
    payload = r.pop("payload")
    if len(payload) == GAME_INFO.size:
        title_id, pid, raw_name, flags = GAME_INFO.unpack(payload)
        r.update({
            "title_id": f"0x{title_id:016X}",
            "pid": pid,
            "process_name": raw_name.rstrip(b"\0").decode("ascii", errors="replace"),
            "flags": flags,
            "read_only": bool(flags & 1),
            "no_gdb": bool(flags & 2),
            "page_mapped": bool(flags & 4),
        })
    else:
        r["payload_hex"] = payload.hex()
    return r


def query(host: str, address: int, timeout: float) -> dict:
    r = request(host, CMD_QUERY, address, timeout=timeout)
    payload = r.pop("payload")
    r["address"] = f"0x{address:08X}"
    if len(payload) == QUERY_INFO.size:
        base, size, perm, state, page_flags = QUERY_INFO.unpack(payload)
        r.update({
            "base": f"0x{base:08X}",
            "size": size,
            "end": f"0x{base + size:08X}",
            "perm": perm,
            "state": state,
            "page_flags": page_flags,
        })
    else:
        r["payload_hex"] = payload.hex()
    return r


def read(host: str, address: int, length: int, timeout: float) -> dict:
    if not 1 <= length <= 512:
        raise BridgeError("length must be 1..512")
    r = request(host, CMD_READ, address, length, timeout)
    payload = r.pop("payload")
    r.update({
        "address": f"0x{address:08X}",
        "requested_length": length,
        "payload_hex": payload.hex(),
    })
    return r


def parse_int(text: str) -> int:
    return int(text, 0)


def dump(obj: dict) -> None:
    print(json.dumps(obj, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description="Pokebot-CFW Rosalina read-only bridge v0p1 probe")
    parser.add_argument("host", help="3DS IPv4 address")
    parser.add_argument("test", choices=["ping", "game", "query", "read", "wild0"])
    parser.add_argument("--address", type=parse_int)
    parser.add_argument("--length", type=parse_int, default=16)
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()

    if args.test == "ping":
        dump(ping(args.host, args.timeout))
    elif args.test == "game":
        dump(game_info(args.host, args.timeout))
    elif args.test == "query":
        if args.address is None:
            parser.error("query requires --address")
        dump(query(args.host, args.address, args.timeout))
    elif args.test == "read":
        if args.address is None:
            parser.error("read requires --address")
        dump(read(args.host, args.address, args.length, args.timeout))
    elif args.test == "wild0":
        # Previously proven ORAS wild-opponent PK6 location. Read only during a wild battle.
        dump(query(args.host, 0x081FFA6C, args.timeout))
        dump(read(args.host, 0x081FFA6C, 232, args.timeout))


if __name__ == "__main__":
    main()
