#pragma once

#include <DvsenseBase/EventBase/EventTypes.hpp>
#include <opencv2/core.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>

class EventFrameRenderer final {
public:
    static constexpr int preview_width = 1280;
    static constexpr int preview_height = 720;

    void initialize(int sensor_width, int sensor_height);
    void add_events(const dvsense::Event2D* begin, const dvsense::Event2D* end);
    void publish_frame();

    bool copy_latest_frame(
        void* destination,
        int destination_capacity,
        int destination_stride,
        std::uint64_t* copied_frame_id) const;

    std::uint64_t frame_id() const { return frame_id_.load(); }

private:
    static const cv::Scalar background_color;
    static const cv::Vec3b on_color;
    static const cv::Vec3b off_color;

    int sensor_width_ = 0;
    int sensor_height_ = 0;
    mutable std::mutex frame_mutex_;
    cv::Mat front_frame_;
    cv::Mat accumulation_frame_;
    std::atomic<std::uint64_t> frame_id_ = 0;
};
