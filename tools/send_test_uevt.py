#!/usr/bin/env python3
"""Send synthetic UEVT V2 event and box streams for receiver/UI testing."""

from __future__ import annotations

import argparse
import math
import socket
import struct
import time
import zlib


HEADER = struct.Struct("!4sBBHHHIIIQQQQHHHHHHI")
EVENT = struct.Struct("!HHIB3s")
BOX = struct.Struct("!QHHHHIB3s")
EVENTS_PER_PACKET = 116
BOXES_PER_PACKET = 58
WINDOW_US = 20_000


def make_events(slot: int, window_id: int, width: int, height: int, count: int) -> list[bytes]:
    center_x = int((window_id * (3 + slot % 3) + slot * 31) % width)
    center_y = int(height * (0.25 + 0.5 * ((slot % 4) / 3.0)))
    events: list[bytes] = []
    for index in range(count):
        angle = index * 0.23 + window_id * 0.07
        radius = 6 + (index % 38)
        x = max(0, min(width - 1, center_x + int(math.cos(angle) * radius)))
        y = max(0, min(height - 1, center_y + int(math.sin(angle) * radius)))
        offset_us = (index * WINDOW_US) // max(1, count)
        polarity = (index + slot + window_id) & 1
        events.append(EVENT.pack(x, y, offset_us, polarity, bytes(3)))
    return events


def make_boxes(slot: int, window_id: int, width: int, height: int) -> list[bytes]:
    center_x = int((window_id * (3 + slot % 3) + slot * 31) % width)
    center_y = int(height * (0.25 + 0.5 * ((slot % 4) / 3.0)))
    half = max(12, min(width, height) // 12)
    x1 = max(0, center_x - half)
    y1 = max(0, center_y - half)
    x2 = min(width, center_x + half + 1)
    y2 = min(height, center_y + half + 1)
    return [BOX.pack(slot + 1, x1, y1, x2, y2, 900_000, 1, bytes(3))]


def _header(
    *,
    kind: int,
    flags: int,
    stride: int,
    sequence: int,
    camera_id: int,
    generation: int,
    session_id: int,
    window_id: int,
    start_us: int,
    width: int,
    height: int,
    fragment: int,
    fragments: int,
    count: int,
    update_id: int,
) -> bytes:
    return HEADER.pack(
        b"UEVT", 2, kind, flags, 72, stride, sequence & 0xFFFFFFFF,
        camera_id, generation, session_id, window_id, start_us,
        start_us + WINDOW_US, width, height, fragment, fragments,
        count, update_id & 0xFFFF, 0,
    )


def _finish_packet(header: bytes, payload: bytes) -> bytes:
    packet = header + payload
    checksum = zlib.crc32(packet) & 0xFFFFFFFF
    return packet[:68] + struct.pack("!I", checksum) + packet[72:]


def build_packets(
    sequence: int,
    window_id: int,
    start_us: int,
    width: int,
    height: int,
    events: list[bytes],
    *,
    camera_id: int = 1,
    generation: int = 1,
    session_id: int = 1,
    update_id: int | None = None,
    boxes: list[bytes] | None = None,
) -> list[bytes]:
    update = (window_id + 1) & 0xFFFF if update_id is None else update_id
    boxes = [] if boxes is None else boxes
    packets: list[bytes] = []
    for kind, stride, maximum, records in (
        (1, EVENT.size, EVENTS_PER_PACKET, events),
        (2, BOX.size, BOXES_PER_PACKET, boxes),
    ):
        chunks = [records[index : index + maximum] for index in range(0, len(records), maximum)] or [[]]
        for fragment_index, fragment in enumerate(chunks):
            payload = b"".join(fragment)
            header = _header(
                kind=kind, flags=2, stride=stride, sequence=sequence,
                camera_id=camera_id, generation=generation, session_id=session_id,
                window_id=window_id, start_us=start_us, width=width, height=height,
                fragment=fragment_index, fragments=len(chunks), count=len(fragment),
                update_id=update,
            )
            packets.append(_finish_packet(header, payload))
            sequence = (sequence + 1) & 0xFFFFFFFF
    return packets


def main() -> None:
    parser = argparse.ArgumentParser(description="Send synthetic UEVT V2 UDP streams")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--base-port", type=int, default=50000)
    parser.add_argument("--slots", type=int, default=4, choices=range(1, 21))
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--events-per-window", type=int, default=600)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sequences = [0] * args.slots
    sessions = [0x5545565400000000 + slot + 1 for slot in range(args.slots)]
    window_id = 0
    deadline = time.perf_counter()
    highest_port = args.base_port + (args.slots - 1) // 2
    print(
        f"Sending {args.slots} synthetic UEVT V2 camera streams to "
        f"{args.host}:{args.base_port}..{highest_port}. Ctrl+C to stop."
    )
    try:
        while True:
            start_us = time.monotonic_ns() // 1_000
            for slot in range(args.slots):
                events = make_events(slot, window_id, args.width, args.height, args.events_per_window)
                boxes = make_boxes(slot, window_id, args.width, args.height)
                packets = build_packets(
                    sequences[slot], window_id, start_us, args.width, args.height, events,
                    camera_id=slot % 2 + 1,
                    session_id=sessions[slot],
                    boxes=boxes,
                )
                target_port = args.base_port + slot // 2
                for packet in packets:
                    sock.sendto(packet, (args.host, target_port))
                sequences[slot] = (sequences[slot] + len(packets)) & 0xFFFFFFFF
            window_id += 1
            deadline += WINDOW_US / 1_000_000
            time.sleep(max(0.0, deadline - time.perf_counter()))
    except KeyboardInterrupt:
        print("Stopped.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
