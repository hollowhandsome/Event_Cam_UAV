#include "UevtProtocol.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

void write_u16(std::vector<std::uint8_t>& bytes, const int offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::vector<std::uint8_t>& bytes, const int offset, const std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void write_u64(std::vector<std::uint8_t>& bytes, const int offset, const std::uint64_t value) {
    for (int index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (56 - index * 8));
}

std::vector<std::uint8_t> make_packet(
    const uevt::MessageType type,
    const std::uint16_t count,
    const std::uint16_t flags = 2) {
    const std::uint16_t stride = type == uevt::MessageType::events
        ? 12 : type == uevt::MessageType::boxes ? 24 : 32;
    std::vector<std::uint8_t> bytes(
        uevt::header_size + static_cast<std::size_t>(stride) * count, 0);
    bytes[0] = 'U'; bytes[1] = 'E'; bytes[2] = 'V'; bytes[3] = 'T';
    bytes[4] = 2;
    bytes[5] = static_cast<std::uint8_t>(type);
    write_u16(bytes, 6, flags);
    write_u16(bytes, 8, 72);
    write_u16(bytes, 10, stride);
    write_u32(bytes, 12, 7);
    write_u32(bytes, 16, 1);
    write_u32(bytes, 20, 3);
    write_u64(bytes, 24, 0x123456789ABCDEF0ULL);
    write_u64(bytes, 32, 9);
    write_u64(bytes, 40, 1'000'000);
    write_u64(bytes, 48, 1'020'000);
    write_u16(bytes, 56, 1280);
    write_u16(bytes, 58, 720);
    write_u16(bytes, 60, 0);
    write_u16(bytes, 62, 1);
    write_u16(bytes, 64, count);
    write_u16(bytes, 66, 4);
    return bytes;
}

void finish_crc(std::vector<std::uint8_t>& bytes) {
    write_u32(bytes, 68, uevt::packet_crc32(bytes));
}

bool expect(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

}

int main(const int argc, char** argv) {
    int failures = 0;
    std::string error;
    uevt::Packet packet;

    auto event_bytes = make_packet(uevt::MessageType::events, 1);
    const int event = static_cast<int>(uevt::header_size);
    write_u16(event_bytes, event, 123);
    write_u16(event_bytes, event + 2, 45);
    write_u32(event_bytes, event + 4, 900);
    event_bytes[event + 8] = 1;
    finish_crc(event_bytes);
    if (!expect(uevt::parse_packet(event_bytes, packet, error), "valid V2 event parses")) ++failures;
    if (!expect(
            packet.camera_id == 1 && packet.generation == 3 &&
            packet.session_id == 0x123456789ABCDEF0ULL && packet.window_id == 9 &&
            packet.update_id == 4,
            "V2 association fields decode")) ++failures;
    if (!expect(
            packet.events.size() == 1 && packet.events[0].x == 123 &&
            packet.events[0].y == 45 && packet.events[0].timestamp_us == 1'000'900 &&
            packet.events[0].polarity == 1,
            "V2 event fields decode")) ++failures;

    auto box_bytes = make_packet(uevt::MessageType::boxes, 1);
    const int box = static_cast<int>(uevt::header_size);
    write_u64(box_bytes, box, 88);
    write_u16(box_bytes, box + 8, 100);
    write_u16(box_bytes, box + 10, 120);
    write_u16(box_bytes, box + 12, 300);
    write_u16(box_bytes, box + 14, 400);
    write_u32(box_bytes, box + 16, 875000);
    box_bytes[box + 20] = 1;
    finish_crc(box_bytes);
    if (!expect(uevt::parse_packet(box_bytes, packet, error), "valid V2 box parses")) ++failures;
    if (!expect(
            packet.boxes.size() == 1 && packet.boxes[0].track_id == 88 &&
            packet.boxes[0].x1 == 100 && packet.boxes[0].y2 == 400 &&
            packet.boxes[0].probability_millionths == 875000 &&
            packet.boxes[0].class_id == 1,
            "V2 box fields decode")) ++failures;

    auto status_bytes = make_packet(uevt::MessageType::status, 1, 0);
    const int status = static_cast<int>(uevt::header_size);
    write_u32(status_bytes, status, 100000);
    write_u64(status_bytes, status + 4, 12);
    write_u64(status_bytes, status + 12, 3);
    write_u32(status_bytes, status + 20, 1);
    finish_crc(status_bytes);
    if (!expect(uevt::parse_packet(status_bytes, packet, error), "valid V2 status parses")) ++failures;
    if (!expect(
            packet.status.threshold_millionths == 100000 &&
            packet.status.dropped_windows == 12 && packet.status.send_errors == 3 &&
            packet.status.enabled,
            "V2 status fields decode")) ++failures;

    auto bad_crc = event_bytes;
    bad_crc.back() ^= 1;
    if (!expect(!uevt::parse_packet(bad_crc, packet, error), "bad whole-packet CRC is rejected")) ++failures;

    auto v1 = event_bytes;
    v1[4] = 1;
    if (!expect(!uevt::parse_packet(v1, packet, error), "V1 packet is rejected")) ++failures;

    auto bad_coordinate = event_bytes;
    write_u16(bad_coordinate, event, 1280);
    finish_crc(bad_coordinate);
    if (!expect(
            !uevt::parse_packet(bad_coordinate, packet, error),
            "out-of-range V2 coordinate is rejected")) ++failures;

    for (int index = 1; index < argc; ++index) {
        std::ifstream stream(argv[index], std::ios::binary);
        const std::vector<std::uint8_t> sample{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        const std::string message = "delivery sample parses: " + std::string(argv[index]);
        if (!expect(
                !sample.empty() && uevt::parse_packet(sample, packet, error),
                message.c_str())) ++failures;
    }

    if (failures == 0) std::cout << "All UEVT V2 protocol tests passed.\n";
    return failures == 0 ? 0 : 1;
}
