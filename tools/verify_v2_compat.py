#!/usr/bin/env python3
"""Cross-check the synthetic sender with the Jetson delivery's V2 decoder."""

from __future__ import annotations

import sys
import time
from pathlib import Path


def main() -> None:
    udp_root = Path(__file__).resolve().parents[1]
    workspace = udp_root.parent
    vendor_integration = (
        workspace
        / "event_cam_studio_developer_thor_arm64_v2_20260924"
        / "integration"
    )
    if not (vendor_integration / "uevt_v2.py").is_file():
        raise RuntimeError(f"Jetson V2 decoder not found: {vendor_integration}")

    sys.path.insert(0, str(vendor_integration))
    from uevt_v2 import Reassembler, decode  # type: ignore[import-not-found]
    from send_test_uevt import build_packets, make_boxes, make_events

    events = make_events(0, 1, 1280, 720, 200)
    boxes = make_boxes(0, 1, 1280, 720)
    packets = build_packets(
        0,
        1,
        time.monotonic_ns() // 1_000,
        1280,
        720,
        events,
        camera_id=1,
        session_id=123,
        boxes=boxes,
    )
    decoded = [decode(packet) for packet in packets]
    if sum(len(item["events"]) for item in decoded) != 200:
        raise RuntimeError("Vendor decoder did not recover all synthetic events")
    if sum(len(item["boxes"]) for item in decoded) != 1:
        raise RuntimeError("Vendor decoder did not recover the synthetic box")
    if {item["kind"] for item in decoded} != {1, 2}:
        raise RuntimeError("Synthetic stream did not contain event and box messages")

    assembler = Reassembler()
    result = None
    for packet in reversed(packets):
        completed = assembler.feed(packet)
        if completed is not None:
            result = completed
    if result is None or len(result["events"]) != 200 or len(result["boxes"]) != 1:
        raise RuntimeError("Vendor reassembler rejected reordered synthetic fragments")

    print(
        f"Vendor UEVT V2 compatibility passed: "
        f"packets={len(packets)}, events={len(result['events'])}, "
        f"boxes={len(result['boxes'])}"
    )


if __name__ == "__main__":
    main()
