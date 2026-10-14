#pragma once

#include "EventTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uevt {

constexpr std::size_t header_size = 72;
constexpr std::size_t event_size = 12;
constexpr std::size_t box_size = 24;
constexpr std::size_t status_size = 32;
constexpr std::size_t maximum_udp_payload = 1472;
constexpr std::uint16_t maximum_events_per_packet = 116;
constexpr std::uint16_t maximum_boxes_per_packet = 58;

enum class MessageType : std::uint8_t {
    events = 1,
    boxes = 2,
    status = 3
};

struct SenderStatus {
    std::uint32_t threshold_millionths = 0;
    std::uint64_t dropped_windows = 0;
    std::uint64_t send_errors = 0;
    bool enabled = false;
};

struct Packet {
    MessageType message_type = MessageType::events;
    std::uint16_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t camera_id = 0;
    std::uint32_t generation = 0;
    std::uint64_t session_id = 0;
    std::uint64_t window_id = 0;
    std::uint16_t fragment_index = 0;
    std::uint16_t fragment_count = 0;
    std::uint16_t update_id = 0;
    std::uint16_t sensor_width = 0;
    std::uint16_t sensor_height = 0;
    std::uint64_t window_start_us = 0;
    std::uint64_t window_end_us = 0;
    std::vector<EventPoint> events;
    std::vector<DetectionBox> boxes;
    SenderStatus status;
};

std::uint32_t crc32(std::span<const std::uint8_t> bytes);
std::uint32_t packet_crc32(std::span<const std::uint8_t> bytes);

bool parse_packet(
    std::span<const std::uint8_t> bytes,
    Packet& packet,
    std::string& error);

}
