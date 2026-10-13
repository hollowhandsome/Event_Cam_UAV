#include "EventCoreApi.h"
#include "EventFrameRenderer.h"

#include <DvsenseDriver/camera/DvsCameraManager.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr int default_frames_per_second = 60;
constexpr int default_frame_interval_microseconds = 16667;

bool valid_display_settings(
    const int width,
    const int height,
    const int frames_per_second) {
    const bool valid_resolution =
        (width == 1280 && height == 720) ||
        (width == 640 && height == 360) ||
        (width == 480 && height == 270) ||
        (width == 320 && height == 180);
    return valid_resolution &&
        (frames_per_second == 60 ||
         frames_per_second == 45 ||
         frames_per_second == 30);
}

int copy_string(const std::string& value, char* buffer, const int capacity) {
    const auto required = static_cast<int>(value.size() + 1);
    if (buffer != nullptr && capacity > 0) {
        const auto copy_count = std::min(capacity - 1, static_cast<int>(value.size()));
        if (copy_count > 0) {
            std::memcpy(buffer, value.data(), copy_count);
        }
        buffer[copy_count] = '\0';
    }
    return required;
}

class CameraSession final {
public:
    ~CameraSession() { stop(); }

    int start(dvsense::DvsCameraManager& manager, const std::string& serial,
              const int bias_off, const int bias_on) {
        stop();
        try {
            std::lock_guard camera_lock(camera_mutex_);
            camera_ = manager.openCamera(serial);
            if (!camera_) {
                set_error("DVSense SDK failed to open camera " + serial + ".");
                return 0;
            }
            sensor_width_ = camera_->getWidth();
            sensor_height_ = camera_->getHeight();
            if (sensor_width_ <= 0 || sensor_height_ <= 0) {
                camera_.reset();
                set_error("Camera " + serial + " reported an invalid sensor size.");
                return 0;
            }
            if (!apply_bias_locked(bias_off, bias_on)) {
                camera_.reset();
                return 0;
            }

            renderer_.initialize(sensor_width_, sensor_height_);
            total_events_.store(0);
            events_per_second_.store(0.0);
            last_rate_count_ = 0;
            last_rate_time_ = std::chrono::steady_clock::now();

            camera_->setBatchEventsNum(10000);
            callback_id_ = camera_->addEventsStreamHandleCallback(
                [this](const dvsense::Event2D* begin, const dvsense::Event2D* end) {
                    process_events(begin, end);
                });
            const int start_result = camera_->start();
            if (start_result != 0) {
                camera_->removeEventsStreamHandleCallback(callback_id_);
                callback_id_ = 0;
                camera_.reset();
                set_error("DVSense SDK failed to start camera " + serial +
                          ". Error code: " + std::to_string(start_result));
                return 0;
            }

            running_.store(true);
            frame_thread_ = std::thread([this] { frame_loop(); });
            set_error("");
            return 1;
        } catch (const std::exception& error) {
            cleanup_camera_locked();
            set_error("Failed to start camera " + serial + ": " + error.what());
            return 0;
        }
    }

    void stop() {
        std::thread frame_thread;
        {
            std::lock_guard camera_lock(camera_mutex_);
            running_.store(false);
            cleanup_camera_locked();
            if (frame_thread_.joinable()) {
                frame_thread = std::move(frame_thread_);
            }
        }
        if (frame_thread.joinable()) {
            frame_thread.join();
        }
        renderer_.deactivate();
    }

    void configure_display(
        const int width,
        const int height,
        const int frames_per_second) {
        std::lock_guard camera_lock(camera_mutex_);
        renderer_.configure_output(width, height);
        const int interval =
            (1'000'000 + frames_per_second / 2) / frames_per_second;
        frame_interval_microseconds_.store(interval);
    }

    int apply_bias(const int bias_off, const int bias_on) {
        try {
            std::lock_guard camera_lock(camera_mutex_);
            if (!camera_) {
                set_error("Start this camera before applying Bias parameters.");
                return 0;
            }
            return apply_bias_locked(bias_off, bias_on) ? 1 : 0;
        } catch (const std::exception& error) {
            set_error(std::string("Failed to apply Bias parameters: ") + error.what());
            return 0;
        }
    }

    int get_status(EventCoreStatus* status) const {
        if (status == nullptr) return 0;
        status->state = running_.load() ? 1 : 0;
        renderer_.get_output_size(&status->width, &status->height);
        status->bias_diff_off = bias_off_.load();
        status->bias_diff_on = bias_on_.load();
        status->total_events = total_events_.load();
        status->events_per_second = events_per_second_.load();
        status->frame_id = renderer_.frame_id();
        return 1;
    }

    int copy_frame(void* destination, const int capacity,
                   const int destination_stride,
                   std::uint64_t* copied_frame_id) const {
        return renderer_.copy_latest_frame(
            destination, capacity, destination_stride, copied_frame_id) ? 1 : 0;
    }

    int get_error(char* buffer, const int capacity) const {
        std::lock_guard error_lock(error_mutex_);
        return copy_string(last_error_, buffer, capacity);
    }

private:
    void cleanup_camera_locked() {
        if (!camera_) {
            callback_id_ = 0;
            return;
        }
        try {
            camera_->stop();
            if (callback_id_ != 0)
                camera_->removeEventsStreamHandleCallback(callback_id_);
        } catch (...) {
        }
        callback_id_ = 0;
        camera_.reset();
    }

    bool apply_bias_locked(const int bias_off, const int bias_on) {
        if (bias_off < 0 || bias_on < 0) {
            set_error("Bias values must be non-negative integers.");
            return false;
        }
        const auto bias = camera_->getTool(dvsense::ToolType::TOOL_BIAS);
        if (!bias) {
            set_error("This camera does not expose the Bias tool.");
            return false;
        }
        if (!bias->setParam("bias_diff_off", bias_off) ||
            !bias->setParam("bias_diff_on", bias_on)) {
            set_error("DVSense SDK rejected one or more Bias values.");
            return false;
        }
        int read_off = -1;
        int read_on = -1;
        if (!bias->getParam("bias_diff_off", read_off) ||
            !bias->getParam("bias_diff_on", read_on) ||
            read_off != bias_off || read_on != bias_on) {
            set_error("Bias readback does not match the requested values.");
            return false;
        }
        bias_off_.store(read_off);
        bias_on_.store(read_on);
        return true;
    }

    void process_events(const dvsense::Event2D* begin, const dvsense::Event2D* end) {
        if (!running_.load() || begin == nullptr || end == nullptr || end < begin) return;
        total_events_.fetch_add(static_cast<std::uint64_t>(end - begin));
        renderer_.add_events(begin, end);
    }

    void frame_loop() {
        auto next_frame = std::chrono::steady_clock::now();
        while (running_.load()) {
            const auto frame_interval = std::chrono::microseconds(
                frame_interval_microseconds_.load());
            next_frame += frame_interval;
            std::this_thread::sleep_until(next_frame);
            if (!running_.load()) break;
            renderer_.publish_frame();
            const auto now = std::chrono::steady_clock::now();
            if (next_frame + frame_interval < now) {
                next_frame = now;
            }
            const auto elapsed = std::chrono::duration<double>(now - last_rate_time_).count();
            if (elapsed >= 1.0) {
                const auto current = total_events_.load();
                events_per_second_.store((current - last_rate_count_) / elapsed);
                last_rate_count_ = current;
                last_rate_time_ = now;
            }
        }
    }

    void set_error(std::string message) {
        std::lock_guard error_lock(error_mutex_);
        last_error_ = std::move(message);
    }

    mutable std::mutex camera_mutex_;
    dvsense::CameraDevice camera_;
    std::uint32_t callback_id_ = 0;
    int sensor_width_ = 0;
    int sensor_height_ = 0;
    EventFrameRenderer renderer_;
    std::thread frame_thread_;
    std::atomic<bool> running_ = false;
    std::atomic<std::uint64_t> total_events_ = 0;
    std::atomic<double> events_per_second_ = 0.0;
    std::uint64_t last_rate_count_ = 0;
    std::chrono::steady_clock::time_point last_rate_time_;
    std::atomic<int> bias_off_ = 0;
    std::atomic<int> bias_on_ = 0;
    std::atomic<int> frame_interval_microseconds_ =
        default_frame_interval_microseconds;
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

class EventCore final {
public:
    EventCore() {
        for (auto& session : sessions_) session = std::make_unique<CameraSession>();
    }
    ~EventCore() { shutdown(); }

    int initialize() {
        try {
            std::lock_guard manager_lock(manager_mutex_);
            if (!manager_) manager_ = std::make_unique<dvsense::DvsCameraManager>();
            set_error("");
            return 1;
        } catch (const std::exception& error) {
            set_error(std::string("Failed to initialize DVSense SDK: ") + error.what());
            return 0;
        }
    }

    void shutdown() {
        stop_all();
        std::lock_guard manager_lock(manager_mutex_);
        manager_.reset();
    }

    int refresh_cameras(char* buffer, const int capacity) {
        try {
            std::lock_guard manager_lock(manager_mutex_);
            ensure_manager();
            manager_->updateCameras();
            const auto descriptions = manager_->getCameraDescs();
            std::ostringstream output;
            for (const auto& description : descriptions)
                output << description.serial << '\t' << description.product << '\t'
                       << description.manufacturer << '\n';
            set_error("");
            return copy_string(output.str(), buffer, capacity);
        } catch (const std::exception& error) {
            set_error(std::string("Failed to enumerate cameras: ") + error.what());
            return -1;
        }
    }

    int start_slot(const int slot, const char* requested_serial,
                   const int bias_off, const int bias_on) {
        if (!valid_slot(slot) || requested_serial == nullptr || *requested_serial == '\0') {
            set_error("A valid slot and camera serial are required.");
            return 0;
        }
        try {
            std::lock_guard manager_lock(manager_mutex_);
            ensure_manager();
            sessions_[slot]->configure_display(
                display_width_.load(),
                display_height_.load(),
                frames_per_second_.load());
            return sessions_[slot]->start(*manager_, requested_serial, bias_off, bias_on);
        } catch (const std::exception& error) {
            set_error(std::string("Failed to open camera slot: ") + error.what());
            return 0;
        }
    }

    void stop_slot(const int slot) { if (valid_slot(slot)) sessions_[slot]->stop(); }
    void stop_all() { for (auto& session : sessions_) session->stop(); }
    int set_display_settings(
        const int width,
        const int height,
        const int frames_per_second) {
        if (!valid_display_settings(width, height, frames_per_second)) {
            set_error("Unsupported display resolution or frame rate.");
            return 0;
        }
        try {
            for (auto& session : sessions_) {
                session->configure_display(width, height, frames_per_second);
            }
            display_width_.store(width);
            display_height_.store(height);
            frames_per_second_.store(frames_per_second);
            set_error("");
            return 1;
        } catch (const std::exception& error) {
            set_error(std::string("Failed to apply display settings: ") + error.what());
            return 0;
        }
    }
    int apply_slot_bias(const int slot, const int off, const int on) {
        return valid_slot(slot) ? sessions_[slot]->apply_bias(off, on) : 0;
    }
    int get_slot_status(const int slot, EventCoreStatus* status) const {
        return valid_slot(slot) ? sessions_[slot]->get_status(status) : 0;
    }
    int copy_slot_frame(const int slot, void* destination, const int capacity,
                        const int stride, std::uint64_t* frame_id) const {
        return valid_slot(slot)
            ? sessions_[slot]->copy_frame(destination, capacity, stride, frame_id) : 0;
    }
    int get_slot_error(const int slot, char* buffer, const int capacity) const {
        return valid_slot(slot) ? sessions_[slot]->get_error(buffer, capacity)
                                : copy_string("Invalid camera slot.", buffer, capacity);
    }
    int get_last_error(char* buffer, const int capacity) const {
        std::lock_guard error_lock(error_mutex_);
        return copy_string(last_error_, buffer, capacity);
    }

private:
    static bool valid_slot(const int slot) {
        return slot >= 0 && slot < EVENTCORE_SLOT_COUNT;
    }
    void ensure_manager() {
        if (!manager_) manager_ = std::make_unique<dvsense::DvsCameraManager>();
    }
    void set_error(std::string message) {
        std::lock_guard error_lock(error_mutex_);
        last_error_ = std::move(message);
    }

    std::unique_ptr<dvsense::DvsCameraManager> manager_;
    mutable std::mutex manager_mutex_;
    std::array<std::unique_ptr<CameraSession>, EVENTCORE_SLOT_COUNT> sessions_;
    std::atomic<int> display_width_ = EventFrameRenderer::default_output_width;
    std::atomic<int> display_height_ = EventFrameRenderer::default_output_height;
    std::atomic<int> frames_per_second_ = default_frames_per_second;
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

EventCore core;
}

extern "C" {
std::int32_t EVENTCORE_CALL ec_initialize() { return core.initialize(); }
void EVENTCORE_CALL ec_shutdown() { core.shutdown(); }
std::int32_t EVENTCORE_CALL ec_refresh_cameras(char* buffer, const std::int32_t capacity) {
    return core.refresh_cameras(buffer, capacity);
}
std::int32_t EVENTCORE_CALL ec_start_camera_slot(
    const std::int32_t slot, const char* serial, const std::int32_t off,
    const std::int32_t on) { return core.start_slot(slot, serial, off, on); }
void EVENTCORE_CALL ec_stop_camera_slot(const std::int32_t slot) { core.stop_slot(slot); }
void EVENTCORE_CALL ec_stop_all_cameras() { core.stop_all(); }
std::int32_t EVENTCORE_CALL ec_set_display_settings(
    const std::int32_t width, const std::int32_t height,
    const std::int32_t frames_per_second) {
    return core.set_display_settings(width, height, frames_per_second);
}
std::int32_t EVENTCORE_CALL ec_apply_slot_bias(
    const std::int32_t slot, const std::int32_t off, const std::int32_t on) {
    return core.apply_slot_bias(slot, off, on);
}
std::int32_t EVENTCORE_CALL ec_get_slot_status(
    const std::int32_t slot, EventCoreStatus* status) {
    return core.get_slot_status(slot, status);
}
std::int32_t EVENTCORE_CALL ec_copy_slot_frame(
    const std::int32_t slot, void* destination, const std::int32_t capacity,
    const std::int32_t stride, std::uint64_t* frame_id) {
    return core.copy_slot_frame(slot, destination, capacity, stride, frame_id);
}
std::int32_t EVENTCORE_CALL ec_get_slot_error(
    const std::int32_t slot, char* buffer, const std::int32_t capacity) {
    return core.get_slot_error(slot, buffer, capacity);
}
std::int32_t EVENTCORE_CALL ec_get_last_error(char* buffer, const std::int32_t capacity) {
    return core.get_last_error(buffer, capacity);
}
}
