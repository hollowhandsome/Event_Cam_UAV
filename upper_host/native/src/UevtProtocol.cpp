#include "UevtProtocol.h"

#include <array>
#include <cstring>

namespace {

constexpr std::uint8_t protocol_version = 2;
constexpr std::uint16_t maximum_fragment_count = 4096;

std::uint16_t read_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_u32(const std::uint8_t* data) {
    return
        (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_u64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value = (value << 8) | data[index];
    return value;
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U
                    ? (value >> 1U) ^ 0xEDB88320U
                    : value >> 1U;
            }
            values[index] = value;
        }
        return values;
    }();
    return table;
}

std::size_t expected_stride(const uevt::MessageType type) {
    switch (type) {
    case uevt::MessageType::events: return uevt::event_size;
    case uevt::MessageType::boxes: return uevt::box_size;
    case uevt::MessageType::status: return uevt::status_size;
    }
    return 0;
}

}

std::uint32_t uevt::crc32(const std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0xFFFFFFFFU;
    const auto& table = crc_table();
    for (const std::uint8_t byte : bytes) {
        value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);
    }
    return value ^ 0xFFFFFFFFU;
}

std::uint32_t uevt::packet_crc32(const std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0xFFFFFFFFU;
    const auto& table = crc_table();
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const std::uint8_t byte = index >= 68 && index < 72 ? 0 : bytes[index];
        value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);
    }
    return value ^ 0xFFFFFFFFU;
}

bool uevt::parse_packet(
    const std::span<const std::uint8_t> bytes,
    Packet& packet,
    std::string& error) {
    error.clear();
    packet = {};

    if (bytes.size() < header_size || bytes.size() > maximum_udp_payload) {
        error = "UEVT V2 datagram length is outside 72..1472 bytes.";
        return false;
    }
    if (std::memcmp(bytes.data(), "UEVT", 4) != 0) {
        error = "UEVT magic does not match.";
        return false;
    }
    if (bytes[4] != protocol_version || bytes[5] < 1 || bytes[5] > 3) {
        error = "Only UEVT V2 event, box and status messages are supported.";
        return false;
    }

    packet.message_type = static_cast<MessageType>(bytes[5]);
    packet.flags = read_u16(bytes.data() + 6);
    const std::uint16_t encoded_header_size = read_u16(bytes.data() + 8);
    const std::uint16_t record_stride = read_u16(bytes.data() + 10);
    packet.sequence = read_u32(bytes.data() + 12);
    packet.camera_id = read_u32(bytes.data() + 16);
    packet.generation = read_u32(bytes.data() + 20);
    packet.session_id = read_u64(bytes.data() + 24);
    packet.window_id = read_u64(bytes.data() + 32);
    packet.window_start_us = read_u64(bytes.data() + 40);
    packet.window_end_us = read_u64(bytes.data() + 48);
    packet.sensor_width = read_u16(bytes.data() + 56);
    packet.sensor_height = read_u16(bytes.data() + 58);
    packet.fragment_index = read_u16(bytes.data() + 60);
    packet.fragment_count = read_u16(bytes.data() + 62);
    const std::uint16_t record_count = read_u16(bytes.data() + 64);
    packet.update_id = read_u16(bytes.data() + 66);
    const std::uint32_t expected_crc = read_u32(bytes.data() + 68);

    if (packet.flags > 2 || encoded_header_size != header_size ||
        record_stride != expected_stride(packet.message_type)) {
        error = "UEVT V2 flags, header size or record stride is invalid.";
        return false;
    }
    if (packet.camera_id != 1 && packet.camera_id != 2) {
        error = "UEVT V2 camera_id must be 1 or 2.";
        return false;
    }
    if (packet.fragment_count == 0 ||
        packet.fragment_count > maximum_fragment_count ||
        packet.fragment_index >= packet.fragment_count) {
        error = "UEVT V2 fragment fields are invalid.";
        return false;
    }
    if (packet.sensor_width == 0 || packet.sensor_height == 0 ||
        packet.window_start_us >= packet.window_end_us ||
        packet.window_end_us > 0x7FFFFFFFFFFFFFFFULL) {
        error = "UEVT V2 dimensions or timestamps are invalid.";
        return false;
    }
    if ((packet.message_type == MessageType::events &&
         record_count > maximum_events_per_packet) ||
        (packet.message_type == MessageType::boxes &&
         record_count > maximum_boxes_per_packet) ||
        (packet.message_type == MessageType::status &&
         (record_count != 1 || packet.fragment_count != 1 ||
          packet.fragment_index != 0))) {
        error = "UEVT V2 record count is invalid for the message type.";
        return false;
    }

    const std::size_t payload_size =
        static_cast<std::size_t>(record_count) * record_stride;
    if (bytes.size() != header_size + payload_size) {
        error = "UEVT V2 datagram length does not match record_count.";
        return false;
    }
    if (packet_crc32(bytes) != expected_crc) {
        error = "UEVT V2 whole-datagram CRC32 does not match.";
        return false;
    }

    const auto* payload = bytes.data() + header_size;
    if (packet.message_type == MessageType::events) {
        packet.events.reserve(record_count);
        const std::uint64_t duration = packet.window_end_us - packet.window_start_us;
        for (std::uint16_t index = 0; index < record_count; ++index) {
            const auto* event = payload + static_cast<std::size_t>(index) * event_size;
            EventPoint point;
            point.x = read_u16(event);
            point.y = read_u16(event + 2);
            const std::uint32_t timestamp_offset = read_u32(event + 4);
            point.timestamp_us = packet.window_start_us + timestamp_offset;
            point.polarity = event[8];
            if (point.x >= packet.sensor_width || point.y >= packet.sensor_height ||
                timestamp_offset >= duration || point.polarity > 1 ||
                event[9] != 0 || event[10] != 0 || event[11] != 0) {
                error = "UEVT V2 event record contains an invalid value.";
                packet = {};
                return false;
            }
            packet.events.push_back(point);
        }
    } else if (packet.message_type == MessageType::boxes) {
        packet.boxes.reserve(record_count);
        for (std::uint16_t index = 0; index < record_count; ++index) {
            const auto* box = payload + static_cast<std::size_t>(index) * box_size;
            DetectionBox detection;
            detection.track_id = read_u64(box);
            detection.x1 = read_u16(box + 8);
            detection.y1 = read_u16(box + 10);
            detection.x2 = read_u16(box + 12);
            detection.y2 = read_u16(box + 14);
            detection.probability_millionths = read_u32(box + 16);
            detection.class_id = box[20];
            if (detection.x1 >= detection.x2 || detection.x2 > packet.sensor_width ||
                detection.y1 >= detection.y2 || detection.y2 > packet.sensor_height ||
                detection.probability_millionths > 1'000'000U ||
                (detection.class_id != 1 && detection.class_id != 2) ||
                (packet.flags != 2 && detection.class_id != 1) ||
                box[21] != 0 || box[22] != 0 || box[23] != 0) {
                error = "UEVT V2 detection record contains an invalid value.";
                packet = {};
                return false;
            }
            packet.boxes.push_back(detection);
        }
    } else {
        packet.status.threshold_millionths = read_u32(payload);
        packet.status.dropped_windows = read_u64(payload + 4);
        packet.status.send_errors = read_u64(payload + 12);
        const std::uint32_t enabled = read_u32(payload + 20);
        const std::uint64_t reserved = read_u64(payload + 24);
        if (packet.status.threshold_millionths > 1'000'000U || enabled > 1 ||
            reserved != 0) {
            error = "UEVT V2 status record contains an invalid value.";
            packet = {};
            return false;
        }
        packet.status.enabled = enabled != 0;
    }
    return true;
}
