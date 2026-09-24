#include "EventFrameRenderer.h"

#include <algorithm>
#include <cstring>

const cv::Scalar EventFrameRenderer::background_color{28, 28, 28};
const cv::Vec3b EventFrameRenderer::on_color{255, 255, 0};
const cv::Vec3b EventFrameRenderer::off_color{0, 165, 255};

void EventFrameRenderer::initialize(const int sensor_width, const int sensor_height) {
    std::lock_guard frame_lock(frame_mutex_);
    sensor_width_ = sensor_width;
    sensor_height_ = sensor_height;
    front_frame_ = cv::Mat(
        preview_height, preview_width, CV_8UC3, background_color);
    accumulation_frame_ = cv::Mat(
        preview_height, preview_width, CV_8UC3, background_color);
    frame_id_.store(0);
}

void EventFrameRenderer::add_events(
    const dvsense::Event2D* begin,
    const dvsense::Event2D* end) {
    if (begin == nullptr || end == nullptr || end < begin ||
        sensor_width_ <= 0 || sensor_height_ <= 0) {
        return;
    }

    std::lock_guard frame_lock(frame_mutex_);
    if (accumulation_frame_.empty()) {
        return;
    }
    for (auto event = begin; event != end; ++event) {
        if (event->x >= sensor_width_ || event->y >= sensor_height_) {
            continue;
        }
        const int x = static_cast<int>(event->x) * preview_width / sensor_width_;
        const int y = static_cast<int>(event->y) * preview_height / sensor_height_;
        auto* row = accumulation_frame_.ptr<cv::Vec3b>(y);
        row[x] = event->polarity != 0 ? on_color : off_color;
    }
}

void EventFrameRenderer::publish_frame() {
    std::lock_guard frame_lock(frame_mutex_);
    if (accumulation_frame_.empty()) {
        return;
    }
    std::swap(front_frame_, accumulation_frame_);
    accumulation_frame_.setTo(background_color);
    frame_id_.fetch_add(1);
}

bool EventFrameRenderer::copy_latest_frame(
    void* destination,
    const int destination_capacity,
    const int destination_stride,
    std::uint64_t* copied_frame_id) const {
    if (destination == nullptr || copied_frame_id == nullptr ||
        destination_stride < preview_width * 3 ||
        destination_capacity < destination_stride * preview_height) {
        return false;
    }

    std::lock_guard frame_lock(frame_mutex_);
    if (front_frame_.empty()) {
        return false;
    }
    auto* output = static_cast<std::uint8_t*>(destination);
    const auto source_stride = static_cast<std::size_t>(preview_width) * 3;
    for (int row = 0; row < preview_height; ++row) {
        std::memcpy(
            output + static_cast<std::size_t>(row) * destination_stride,
            front_frame_.ptr(row),
            source_stride);
    }
    *copied_frame_id = frame_id_.load();
    return true;
}
