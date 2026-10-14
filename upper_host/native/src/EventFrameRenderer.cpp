#include "EventFrameRenderer.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>

const cv::Scalar EventFrameRenderer::background_color{28, 28, 28};
const cv::Vec3b EventFrameRenderer::on_color{255, 255, 0};
const cv::Vec3b EventFrameRenderer::off_color{0, 165, 255};

void EventFrameRenderer::initialize(const int sensor_width, const int sensor_height) {
    std::lock_guard frame_lock(frame_mutex_);
    if (sensor_width_ == sensor_width && sensor_height_ == sensor_height &&
        !front_frame_.empty()) {
        return;
    }
    sensor_width_ = sensor_width;
    sensor_height_ = sensor_height;
    reset_frames_locked();
    frame_id_.fetch_add(1);
}

void EventFrameRenderer::configure_output(
    const int output_width,
    const int output_height) {
    if (output_width <= 0 || output_height <= 0) return;
    std::lock_guard frame_lock(frame_mutex_);
    if (output_width_ == output_width && output_height_ == output_height) return;
    output_width_ = output_width;
    output_height_ = output_height;
    if (sensor_width_ > 0 && sensor_height_ > 0) {
        reset_frames_locked();
        frame_id_.fetch_add(1);
    }
}

void EventFrameRenderer::deactivate() {
    std::lock_guard frame_lock(frame_mutex_);
    sensor_width_ = 0;
    sensor_height_ = 0;
    front_frame_.release();
    accumulation_frame_.release();
    boxes_.clear();
    frame_id_.fetch_add(1);
}

void EventFrameRenderer::set_boxes(const std::vector<DetectionBox>& boxes) {
    std::lock_guard frame_lock(frame_mutex_);
    boxes_ = boxes;
}

void EventFrameRenderer::clear_boxes() {
    std::lock_guard frame_lock(frame_mutex_);
    boxes_.clear();
}

void EventFrameRenderer::reset_frames_locked() {
    front_frame_ = cv::Mat(
        output_height_, output_width_, CV_8UC3, background_color);
    accumulation_frame_ = cv::Mat(
        output_height_, output_width_, CV_8UC3, background_color);
}

void EventFrameRenderer::add_events(
    const EventPoint* begin,
    const EventPoint* end) {
    if (begin == nullptr || end == nullptr || end < begin) return;

    std::lock_guard frame_lock(frame_mutex_);
    if (sensor_width_ <= 0 || sensor_height_ <= 0 ||
        accumulation_frame_.empty()) return;

    for (auto event = begin; event != end; ++event) {
        if (event->x >= sensor_width_ || event->y >= sensor_height_) continue;
        const int x = std::min(output_width_ - 1, static_cast<int>(
            static_cast<std::int64_t>(event->x) * output_width_ / sensor_width_));
        const int y = std::min(output_height_ - 1, static_cast<int>(
            static_cast<std::int64_t>(event->y) * output_height_ / sensor_height_));
        accumulation_frame_.ptr<cv::Vec3b>(y)[x] =
            event->polarity != 0 ? on_color : off_color;
    }
}

void EventFrameRenderer::get_output_size(
    int* output_width,
    int* output_height) const {
    std::lock_guard frame_lock(frame_mutex_);
    if (output_width != nullptr) *output_width = output_width_;
    if (output_height != nullptr) *output_height = output_height_;
}

void EventFrameRenderer::publish_frame() {
    std::lock_guard frame_lock(frame_mutex_);
    if (accumulation_frame_.empty()) return;
    std::swap(front_frame_, accumulation_frame_);
    for (const auto& box : boxes_) {
        const int left = std::clamp(
            static_cast<int>(static_cast<std::int64_t>(box.x1) * output_width_ /
                sensor_width_), 0, output_width_ - 1);
        const int top = std::clamp(
            static_cast<int>(static_cast<std::int64_t>(box.y1) * output_height_ /
                sensor_height_), 0, output_height_ - 1);
        const int right = std::clamp(
            static_cast<int>(static_cast<std::int64_t>(box.x2) * output_width_ /
                sensor_width_), left + 1, output_width_);
        const int bottom = std::clamp(
            static_cast<int>(static_cast<std::int64_t>(box.y2) * output_height_ /
                sensor_height_), top + 1, output_height_);
        const cv::Scalar color = box.class_id == 1
            ? cv::Scalar(60, 60, 255)
            : cv::Scalar(170, 170, 170);
        cv::rectangle(
            front_frame_,
            cv::Rect(left, top, right - left, bottom - top),
            color,
            std::max(1, output_width_ / 640));
    }
    accumulation_frame_.setTo(background_color);
    frame_id_.fetch_add(1);
}

bool EventFrameRenderer::copy_latest_frame(
    void* destination,
    const int destination_capacity,
    const int destination_stride,
    std::uint64_t* copied_frame_id) const {
    if (destination == nullptr || copied_frame_id == nullptr) return false;

    std::lock_guard frame_lock(frame_mutex_);
    if (front_frame_.empty() || destination_stride < output_width_ * 3 ||
        destination_capacity < destination_stride * output_height_) return false;

    auto* output = static_cast<std::uint8_t*>(destination);
    const auto source_stride = static_cast<std::size_t>(output_width_) * 3;
    for (int row = 0; row < output_height_; ++row) {
        std::memcpy(
            output + static_cast<std::size_t>(row) * destination_stride,
            front_frame_.ptr(row),
            source_stride);
    }
    *copied_frame_id = frame_id_.load();
    return true;
}
