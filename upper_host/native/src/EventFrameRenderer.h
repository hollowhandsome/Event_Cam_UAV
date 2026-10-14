#pragma once

#include "EventTypes.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class EventFrameRenderer final {
public:
    static constexpr int default_output_width = 1280;
    static constexpr int default_output_height = 720;

    void initialize(int sensor_width, int sensor_height);
    void configure_output(int output_width, int output_height);
    void deactivate();
    void add_events(const EventPoint* begin, const EventPoint* end);
    void set_boxes(const std::vector<DetectionBox>& boxes);
    void clear_boxes();
    void publish_frame();
    void get_output_size(int* output_width, int* output_height) const;

    bool copy_latest_frame(
        void* destination,
        int destination_capacity,
        int destination_stride,
        std::uint64_t* copied_frame_id) const;

    std::uint64_t frame_id() const { return frame_id_.load(); }

private:
    void reset_frames_locked();

    static const cv::Scalar background_color;
    static const cv::Vec3b on_color;
    static const cv::Vec3b off_color;

    int sensor_width_ = 0;
    int sensor_height_ = 0;
    int output_width_ = default_output_width;
    int output_height_ = default_output_height;
    mutable std::mutex frame_mutex_;
    cv::Mat front_frame_;
    cv::Mat accumulation_frame_;
    std::vector<DetectionBox> boxes_;
    std::atomic<std::uint64_t> frame_id_ = 0;
};
