#pragma once

#include <cstdint>

struct EventPoint {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint64_t timestamp_us = 0;
    std::uint8_t polarity = 0;
};

struct DetectionBox {
    std::uint64_t track_id = 0;
    std::uint16_t x1 = 0;
    std::uint16_t y1 = 0;
    std::uint16_t x2 = 0;
    std::uint16_t y2 = 0;
    std::uint32_t probability_millionths = 0;
    std::uint8_t class_id = 0;
};
