#!/usr/bin/env python3
"""End-to-end smoke test for EventCore.dll using localhost UDP."""

from __future__ import annotations

import ctypes
import socket
import sys
import time
from pathlib import Path

from send_test_uevt import BOX, EVENT, WINDOW_US, build_packets, make_boxes, make_events


class Status(ctypes.Structure):
    _fields_ = [
        ("state", ctypes.c_int32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("listen_port", ctypes.c_int32),
        ("total_events", ctypes.c_uint64),
        ("received_packets", ctypes.c_uint64),
        ("lost_packets", ctypes.c_uint64),
        ("out_of_order_packets", ctypes.c_uint64),
        ("invalid_packets", ctypes.c_uint64),
        ("incomplete_windows", ctypes.c_uint64),
        ("events_per_second", ctypes.c_double),
        ("frame_id", ctypes.c_uint64),
    ]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    binary_dir = root / "upper_host" / "ui" / "bin" / "Release" / "net8.0-windows"
    dll_path = binary_dir / "EventCore.dll"
    require(dll_path.is_file(), "EventCore.dll is missing; build UDP_ver first")
    if hasattr(os := __import__("os"), "add_dll_directory"):
        os.add_dll_directory(str(binary_dir))
    core = ctypes.CDLL(str(dll_path))

    core.ec_initialize.restype = ctypes.c_int32
    core.ec_set_network_settings.argtypes = [ctypes.c_char_p, ctypes.c_int32]
    core.ec_set_network_settings.restype = ctypes.c_int32
    core.ec_set_display_settings.argtypes = [ctypes.c_int32, ctypes.c_int32, ctypes.c_int32]
    core.ec_set_display_settings.restype = ctypes.c_int32
    core.ec_set_slot_display_mode.argtypes = [ctypes.c_int32, ctypes.c_int32]
    core.ec_set_slot_display_mode.restype = ctypes.c_int32
    core.ec_start_udp_slot.argtypes = [ctypes.c_int32]
    core.ec_start_udp_slot.restype = ctypes.c_int32
    core.ec_stop_udp_slot.argtypes = [ctypes.c_int32]
    core.ec_get_slot_status.argtypes = [ctypes.c_int32, ctypes.POINTER(Status)]
    core.ec_get_slot_status.restype = ctypes.c_int32
    core.ec_copy_slot_frame.argtypes = [
        ctypes.c_int32,
        ctypes.c_void_p,
        ctypes.c_int32,
        ctypes.c_int32,
        ctypes.POINTER(ctypes.c_uint64),
    ]
    core.ec_copy_slot_frame.restype = ctypes.c_int32

    require(ctypes.sizeof(Status) == 80, "Status layout size mismatch")
    require(core.ec_initialize() == 1, "ec_initialize failed")
    test_port = 55120
    try:
        require(
            core.ec_set_network_settings(b"127.0.0.1", test_port) == 1,
            "network configuration failed",
        )
        require(core.ec_set_display_settings(320, 180, 30) == 1, "display setup failed")
        require(core.ec_set_slot_display_mode(0, 0) == 1, "raw display mode failed")
        require(core.ec_set_slot_display_mode(0, 2) == 0, "invalid display mode was accepted")
        require(core.ec_start_udp_slot(0) == 1, "camera_id=1 slot failed to start")
        require(core.ec_start_udp_slot(1) == 1, "camera_id=2 slot failed to start")

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(b"invalid", ("127.0.0.1", test_port))
        sequences = [0, 0]
        window_id = 0
        deadline = time.perf_counter() + 1.0
        while time.perf_counter() < deadline:
            start_us = time.monotonic_ns() // 1_000
            for camera_index in range(2):
                events = make_events(camera_index, window_id, 1280, 720, 600)
                boxes = make_boxes(camera_index, window_id, 1280, 720)
                packets = build_packets(
                    sequences[camera_index], window_id, start_us, 1280, 720, events,
                    camera_id=camera_index + 1,
                    session_id=100 + camera_index,
                    boxes=boxes,
                )
                for packet in packets:
                    sock.sendto(packet, ("127.0.0.1", test_port))
                sequences[camera_index] += len(packets)
            window_id += 1
            time.sleep(WINDOW_US / 1_000_000)

        statuses = [Status(), Status()]
        for slot, status in enumerate(statuses):
            require(core.ec_get_slot_status(slot, ctypes.byref(status)) == 1, "status call failed")
            require(status.state == 1, f"slot {slot} is not running")
            require(status.total_events > 0, f"camera_id={slot + 1} events did not reach renderer")
            require(status.received_packets > 0, f"camera_id={slot + 1} packets were not counted")
            require(status.width == 320 and status.height == 180, "output size mismatch")
        status = statuses[0]
        print(
            "Smoke-test status: "
            f"cam1_packets={statuses[0].received_packets}, cam1_events={statuses[0].total_events}, "
            f"cam2_packets={statuses[1].received_packets}, cam2_events={statuses[1].total_events}, "
            f"invalid={statuses[0].invalid_packets + statuses[1].invalid_packets}"
        )
        require(
            statuses[0].invalid_packets + statuses[1].invalid_packets >= 1,
            "invalid packet was not rejected",
        )

        stride = status.width * 3
        frame = (ctypes.c_uint8 * (stride * status.height))()
        frame_id = ctypes.c_uint64()
        require(
            core.ec_copy_slot_frame(
                0, frame, len(frame), stride, ctypes.byref(frame_id)
            )
            == 1,
            "no rendered frame was available",
        )
        require(any(value != 28 for value in frame), "rendered frame contains only background")

        def send_filter_probe(mode: int) -> tuple[bool, bool]:
            nonlocal window_id
            require(core.ec_set_slot_display_mode(0, mode) == 1, "display mode switch failed")
            probe_events = [
                EVENT.pack(100, 100, 1_000, 1, bytes(3)),
                EVENT.pack(1_000, 600, 2_000, 1, bytes(3)),
            ]
            probe_boxes = [BOX.pack(99, 80, 80, 200, 200, 950_000, 1, bytes(3))]
            inside_seen = False
            outside_seen = False
            for _ in range(20):
                packets = build_packets(
                    sequences[0], window_id, time.monotonic_ns() // 1_000,
                    1280, 720, probe_events, camera_id=1, session_id=100,
                    boxes=probe_boxes,
                )
                for packet in packets:
                    sock.sendto(packet, ("127.0.0.1", test_port))
                sequences[0] += len(packets)
                window_id += 1
                time.sleep(0.025)
                probe_status = Status()
                require(
                    core.ec_get_slot_status(0, ctypes.byref(probe_status)) == 1,
                    "probe status call failed",
                )
                probe_frame = (ctypes.c_uint8 * (stride * probe_status.height))()
                probe_frame_id = ctypes.c_uint64()
                require(
                    core.ec_copy_slot_frame(
                        0, probe_frame, len(probe_frame), stride,
                        ctypes.byref(probe_frame_id),
                    ) == 1,
                    "probe frame was unavailable",
                )
                for sensor_x, sensor_y, is_inside in ((100, 100, True), (1_000, 600, False)):
                    output_x = sensor_x * probe_status.width // 1_280
                    output_y = sensor_y * probe_status.height // 720
                    offset = output_y * stride + output_x * 3
                    colored = tuple(
                        probe_frame[offset + channel] for channel in range(3)
                    ) != (28, 28, 28)
                    if is_inside:
                        inside_seen |= colored
                    else:
                        outside_seen |= colored
            return inside_seen, outside_seen

        raw_inside_seen, raw_outside_seen = send_filter_probe(0)
        filtered_inside_seen, filtered_outside_seen = send_filter_probe(1)
        require(raw_inside_seen, "raw mode did not render the inside event")
        require(raw_outside_seen, "raw mode removed an outside event")
        require(filtered_inside_seen, "filtered mode removed an event inside the drone box")
        require(not filtered_outside_seen, "filtered mode kept an event outside the drone box")
        print("Display mode test passed: raw and drone-box filtering differ as expected")

        anomaly_events = make_events(0, window_id, 1280, 720, 600)
        anomaly_packets = build_packets(
            sequences[0] + 1,
            window_id,
            time.monotonic_ns() // 1_000,
            1280,
            720,
            anomaly_events,
            camera_id=1,
            session_id=100,
            boxes=make_boxes(0, window_id, 1280, 720),
        )
        sock.sendto(anomaly_packets[0], ("127.0.0.1", test_port))
        sock.sendto(anomaly_packets[0], ("127.0.0.1", test_port))
        time.sleep(2.2)
        require(core.ec_get_slot_status(0, ctypes.byref(status)) == 1, "final status call failed")
        require(status.lost_packets >= 1, "sequence gap was not counted")
        require(status.out_of_order_packets >= 1, "duplicate packet was not counted")
        require(status.incomplete_windows >= 1, "incomplete window was not timed out")
        print(
            "UDP smoke test passed: "
            f"packets={status.received_packets}, events={status.total_events}, "
            f"lost={status.lost_packets}, out_of_order={status.out_of_order_packets}, "
            f"invalid={status.invalid_packets}, incomplete={status.incomplete_windows}, "
            f"frame={frame_id.value}"
        )
        sock.close()
    finally:
        core.ec_stop_udp_slot(0)
        core.ec_stop_udp_slot(1)
        core.ec_shutdown()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"UDP smoke test failed: {error}", file=sys.stderr)
        raise
