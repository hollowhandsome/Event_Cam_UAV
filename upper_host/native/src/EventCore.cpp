#include "EventCoreApi.h"
#include "EventFrameRenderer.h"
#include "UevtProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr int default_base_port = 50000;
constexpr int default_frames_per_second = 60;
constexpr int default_frame_interval_microseconds = 16667;
constexpr int socket_receive_buffer_bytes = 8 * 1024 * 1024;
constexpr DWORD socket_receive_timeout_ms = 100;
constexpr int camera_count_per_group = 2;
constexpr int group_count = EVENTCORE_SLOT_COUNT / camera_count_per_group;
constexpr int display_mode_raw = 0;
constexpr int display_mode_filtered = 1;
constexpr std::uint8_t drone_class_id = 1;
constexpr std::size_t maximum_pending_windows = 256;
constexpr std::size_t maximum_pending_bytes = 64 * 1024 * 1024;
constexpr std::size_t sequence_horizon = 8192;
constexpr auto incomplete_window_timeout = std::chrono::seconds(2);
constexpr auto stale_box_timeout = std::chrono::milliseconds(200);

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
    const int required = static_cast<int>(value.size() + 1);
    if (buffer != nullptr && capacity > 0) {
        const int count = std::min(capacity - 1, static_cast<int>(value.size()));
        if (count > 0) std::memcpy(buffer, value.data(), count);
        buffer[count] = '\0';
    }
    return required;
}

struct AssemblyKey {
    std::uint64_t window_id = 0;
    std::uint16_t update_id = 0;
    std::uint16_t flags = 0;

    bool operator==(const AssemblyKey&) const = default;
};

struct AssemblyKeyHash {
    std::size_t operator()(const AssemblyKey& key) const noexcept {
        std::size_t result = std::hash<std::uint64_t>{}(key.window_id);
        result ^= static_cast<std::size_t>(key.update_id) << 1;
        result ^= static_cast<std::size_t>(key.flags) << 17;
        return result;
    }
};

struct WindowAssembly {
    std::uint16_t sensor_width = 0;
    std::uint16_t sensor_height = 0;
    std::uint64_t window_start_us = 0;
    std::uint64_t window_end_us = 0;
    std::uint16_t event_fragment_count = 0;
    std::uint16_t box_fragment_count = 0;
    std::size_t received_event_fragments = 0;
    std::size_t received_box_fragments = 0;
    std::vector<std::vector<EventPoint>> event_fragments;
    std::vector<std::vector<DetectionBox>> box_fragments;
    std::vector<bool> event_received;
    std::vector<bool> box_received;
    std::size_t payload_bytes = 0;
    std::chrono::steady_clock::time_point first_seen;
};

struct ChannelState {
    std::atomic<bool> enabled = false;
    std::atomic<int> listen_port = 0;
    std::atomic<int> display_mode = display_mode_raw;
    EventFrameRenderer renderer;
    std::mutex protocol_mutex;

    bool stream_initialized = false;
    std::uint64_t active_session = 0;
    std::uint32_t active_generation = 0;
    std::deque<std::uint64_t> retired_sessions;
    bool sequence_initialized = false;
    std::uint32_t highest_sequence = 0;
    std::unordered_set<std::uint32_t> recent_sequences;
    std::deque<std::uint32_t> recent_sequence_order;
    std::unordered_set<std::uint32_t> missing_sequences;
    std::unordered_map<AssemblyKey, WindowAssembly, AssemblyKeyHash> assemblies;
    std::unordered_map<AssemblyKey, std::chrono::steady_clock::time_point, AssemblyKeyHash>
        completed;
    std::size_t pending_payload_bytes = 0;
    std::chrono::steady_clock::time_point last_current_result{};
    bool boxes_visible = false;
    int sensor_width = 0;
    int sensor_height = 0;

    std::atomic<std::uint64_t> total_events = 0;
    std::atomic<std::uint64_t> received_packets = 0;
    std::atomic<std::uint64_t> lost_packets = 0;
    std::atomic<std::uint64_t> out_of_order_packets = 0;
    std::atomic<std::uint64_t> invalid_packets = 0;
    std::atomic<std::uint64_t> incomplete_windows = 0;
    std::atomic<double> events_per_second = 0.0;
};

class UdpGroupSession final {
public:
    ~UdpGroupSession() { stop_all(); }

    void configure_display(
        const int width,
        const int height,
        const int frames_per_second) {
        for (auto& channel : channels_) channel.renderer.configure_output(width, height);
        const int interval =
            (1'000'000 + frames_per_second / 2) / frames_per_second;
        frame_interval_microseconds_.store(interval);
    }

    void set_listen_port(const int port) {
        for (auto& channel : channels_) channel.listen_port.store(port);
    }

    bool is_running() const { return running_.load(); }

    int start_channel(
        const int camera_index,
        const std::string& bind_address,
        const int port) {
        if (!valid_camera(camera_index)) return 0;
        std::lock_guard lifecycle_lock(lifecycle_mutex_);

        ChannelState& channel = channels_[camera_index];
        if (!channel.enabled.load()) {
            reset_channel(channel, true);
            channel.listen_port.store(port);
            channel.enabled.store(true);
        }
        if (running_.load()) return 1;

        SOCKET udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_socket == INVALID_SOCKET) {
            channel.enabled.store(false);
            return fail_with_winsock("Failed to create UDP socket");
        }
        setsockopt(
            udp_socket, SOL_SOCKET, SO_RCVBUF,
            reinterpret_cast<const char*>(&socket_receive_buffer_bytes),
            sizeof(socket_receive_buffer_bytes));
        setsockopt(
            udp_socket, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&socket_receive_timeout_ms),
            sizeof(socket_receive_timeout_ms));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        if (InetPtonA(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
            closesocket(udp_socket);
            channel.enabled.store(false);
            set_error("Bind address must be an IPv4 literal, for example 0.0.0.0.");
            return 0;
        }
        if (bind(
                udp_socket,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
            channel.enabled.store(false);
            return fail_with_winsock(
                "Failed to bind UDP port " + std::to_string(port), udp_socket);
        }

        socket_ = udp_socket;
        set_listen_port(port);
        set_error("");
        running_.store(true);
        receive_thread_ = std::thread([this, udp_socket] { receive_loop(udp_socket); });
        frame_thread_ = std::thread([this] { frame_loop(); });
        return 1;
    }

    void stop_channel(const int camera_index) {
        if (!valid_camera(camera_index)) return;
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        ChannelState& channel = channels_[camera_index];
        channel.enabled.store(false);
        reset_channel(channel, false);
        if (channels_[0].enabled.load() || channels_[1].enabled.load()) return;
        stop_receiver_locked();
    }

    void stop_all() {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        for (auto& channel : channels_) channel.enabled.store(false);
        stop_receiver_locked();
        for (auto& channel : channels_) reset_channel(channel, false);
    }

    int get_status(const int camera_index, EventCoreStatus* status) const {
        if (!valid_camera(camera_index) || status == nullptr) return 0;
        const ChannelState& channel = channels_[camera_index];
        status->state = running_.load() && channel.enabled.load() ? 1 : 0;
        channel.renderer.get_output_size(&status->width, &status->height);
        status->listen_port = channel.listen_port.load();
        status->total_events = channel.total_events.load();
        status->received_packets = channel.received_packets.load();
        status->lost_packets = channel.lost_packets.load();
        status->out_of_order_packets = channel.out_of_order_packets.load();
        status->invalid_packets = channel.invalid_packets.load();
        status->incomplete_windows = channel.incomplete_windows.load();
        status->events_per_second = channel.events_per_second.load();
        status->frame_id = channel.renderer.frame_id();
        return 1;
    }

    int copy_frame(
        const int camera_index,
        void* destination,
        const int capacity,
        const int stride,
        std::uint64_t* frame_id) const {
        return valid_camera(camera_index) &&
            channels_[camera_index].renderer.copy_latest_frame(
                destination, capacity, stride, frame_id) ? 1 : 0;
    }

    int copy_error(char* buffer, const int capacity) const {
        std::lock_guard error_lock(error_mutex_);
        return copy_string(last_error_, buffer, capacity);
    }

    int set_display_mode(const int camera_index, const int mode) {
        if (!valid_camera(camera_index) ||
            (mode != display_mode_raw && mode != display_mode_filtered))
            return 0;
        ChannelState& channel = channels_[camera_index];
        std::lock_guard protocol_lock(channel.protocol_mutex);
        channel.display_mode.store(mode);
        channel.renderer.deactivate();
        if (channel.sensor_width > 0 && channel.sensor_height > 0)
            channel.renderer.initialize(channel.sensor_width, channel.sensor_height);
        channel.boxes_visible = false;
        return 1;
    }

private:
    static bool valid_camera(const int camera_index) {
        return camera_index >= 0 && camera_index < camera_count_per_group;
    }

    int fail_with_winsock(
        const std::string& message,
        const SOCKET socket_to_close = INVALID_SOCKET) {
        const int error_code = WSAGetLastError();
        if (socket_to_close != INVALID_SOCKET) closesocket(socket_to_close);
        set_error(message + ". Winsock error: " + std::to_string(error_code));
        return 0;
    }

    void stop_receiver_locked() {
        running_.store(false);
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (receive_thread_.joinable()) receive_thread_.join();
        if (frame_thread_.joinable()) frame_thread_.join();
    }

    static void clear_protocol_state(ChannelState& channel) {
        channel.stream_initialized = false;
        channel.active_session = 0;
        channel.active_generation = 0;
        channel.retired_sessions.clear();
        channel.sequence_initialized = false;
        channel.highest_sequence = 0;
        channel.recent_sequences.clear();
        channel.recent_sequence_order.clear();
        channel.missing_sequences.clear();
        channel.assemblies.clear();
        channel.completed.clear();
        channel.pending_payload_bytes = 0;
        channel.last_current_result = {};
        channel.boxes_visible = false;
        channel.sensor_width = 0;
        channel.sensor_height = 0;
    }

    static void reset_channel(ChannelState& channel, const bool keep_renderer_ready) {
        std::lock_guard protocol_lock(channel.protocol_mutex);
        clear_protocol_state(channel);
        channel.total_events.store(0);
        channel.received_packets.store(0);
        channel.lost_packets.store(0);
        channel.out_of_order_packets.store(0);
        channel.invalid_packets.store(0);
        channel.incomplete_windows.store(0);
        channel.events_per_second.store(0.0);
        if (!keep_renderer_ready) channel.renderer.deactivate();
        else channel.renderer.clear_boxes();
    }

    void receive_loop(const SOCKET udp_socket) {
        std::array<std::uint8_t, 2048> buffer{};
        while (running_.load()) {
            sockaddr_in source{};
            int source_length = sizeof(source);
            const int received = recvfrom(
                udp_socket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()),
                0,
                reinterpret_cast<sockaddr*>(&source),
                &source_length);

            if (received == SOCKET_ERROR) {
                const int error_code = WSAGetLastError();
                if (!running_.load()) break;
                if (error_code == WSAETIMEDOUT || error_code == WSAEWOULDBLOCK) {
                    purge_expired_windows();
                    continue;
                }
                if (error_code == WSAEMSGSIZE) {
                    add_invalid_to_enabled();
                    set_error("UDP datagram exceeds the receiver buffer size.");
                    continue;
                }
                set_error("UDP receive failed. Winsock error: " +
                          std::to_string(error_code));
                running_.store(false);
                break;
            }

            uevt::Packet packet;
            std::string parse_error;
            const auto bytes = std::span<const std::uint8_t>(
                buffer.data(), static_cast<std::size_t>(received));
            if (!uevt::parse_packet(bytes, packet, parse_error)) {
                int camera_index = -1;
                if (bytes.size() >= 20 && std::memcmp(bytes.data(), "UEVT", 4) == 0 &&
                    bytes[4] == 2) {
                    const std::uint32_t camera_id =
                        (static_cast<std::uint32_t>(bytes[16]) << 24) |
                        (static_cast<std::uint32_t>(bytes[17]) << 16) |
                        (static_cast<std::uint32_t>(bytes[18]) << 8) |
                        static_cast<std::uint32_t>(bytes[19]);
                    if (camera_id == 1 || camera_id == 2)
                        camera_index = static_cast<int>(camera_id - 1);
                }
                if (camera_index >= 0 && channels_[camera_index].enabled.load())
                    channels_[camera_index].invalid_packets.fetch_add(1);
                else
                    add_invalid_to_enabled();
                set_error(parse_error);
                continue;
            }

            const int camera_index = static_cast<int>(packet.camera_id - 1);
            ChannelState& channel = channels_[camera_index];
            if (!channel.enabled.load()) continue;
            channel.received_packets.fetch_add(1);
            std::lock_guard protocol_lock(channel.protocol_mutex);
            if (!channel.enabled.load()) continue;
            if (!accept_stream(channel, packet)) continue;
            if (!accept_sequence(channel, packet.sequence)) continue;
            process_packet(channel, std::move(packet));
            purge_expired_windows_locked(channel, std::chrono::steady_clock::now());
        }
    }

    void add_invalid_to_enabled() {
        for (auto& channel : channels_) {
            if (channel.enabled.load()) channel.invalid_packets.fetch_add(1);
        }
    }

    static bool is_retired_session(
        const ChannelState& channel,
        const std::uint64_t session) {
        return std::find(
            channel.retired_sessions.begin(),
            channel.retired_sessions.end(),
            session) != channel.retired_sessions.end();
    }

    static void reset_for_new_stream(
        ChannelState& channel,
        const uevt::Packet& packet,
        const bool retire_previous) {
        if (retire_previous && channel.stream_initialized) {
            channel.retired_sessions.push_back(channel.active_session);
            if (channel.retired_sessions.size() > 16)
                channel.retired_sessions.pop_front();
        }
        channel.stream_initialized = true;
        channel.active_session = packet.session_id;
        channel.active_generation = packet.generation;
        channel.sequence_initialized = false;
        channel.recent_sequences.clear();
        channel.recent_sequence_order.clear();
        channel.missing_sequences.clear();
        channel.assemblies.clear();
        channel.completed.clear();
        channel.sensor_width = packet.sensor_width;
        channel.sensor_height = packet.sensor_height;
        channel.renderer.initialize(packet.sensor_width, packet.sensor_height);
        channel.renderer.clear_boxes();
        channel.boxes_visible = false;
    }

    static bool accept_stream(ChannelState& channel, const uevt::Packet& packet) {
        if (!channel.stream_initialized) {
            reset_for_new_stream(channel, packet, false);
            return true;
        }
        if (packet.session_id != channel.active_session) {
            if (is_retired_session(channel, packet.session_id)) {
                channel.out_of_order_packets.fetch_add(1);
                return false;
            }
            reset_for_new_stream(channel, packet, true);
            return true;
        }
        if (packet.generation != channel.active_generation) {
            const std::uint32_t forward = packet.generation - channel.active_generation;
            if (forward != 0 && forward < 0x80000000U) {
                reset_for_new_stream(channel, packet, false);
                return true;
            }
            channel.out_of_order_packets.fetch_add(1);
            return false;
        }
        if (packet.sensor_width != channel.sensor_width ||
            packet.sensor_height != channel.sensor_height) {
            channel.invalid_packets.fetch_add(1);
            return false;
        }
        return true;
    }

    static void remember_sequence(ChannelState& channel, const std::uint32_t sequence) {
        if (!channel.recent_sequences.insert(sequence).second) return;
        channel.recent_sequence_order.push_back(sequence);
        while (channel.recent_sequence_order.size() > sequence_horizon) {
            channel.recent_sequences.erase(channel.recent_sequence_order.front());
            channel.recent_sequence_order.pop_front();
        }
        for (auto iterator = channel.missing_sequences.begin();
             iterator != channel.missing_sequences.end();) {
            const std::uint32_t age = channel.highest_sequence - *iterator;
            if (age >= sequence_horizon && age < 0x80000000U)
                iterator = channel.missing_sequences.erase(iterator);
            else
                ++iterator;
        }
    }

    static bool accept_sequence(ChannelState& channel, const std::uint32_t sequence) {
        if (!channel.sequence_initialized) {
            channel.sequence_initialized = true;
            channel.highest_sequence = sequence;
            remember_sequence(channel, sequence);
            return true;
        }
        if (channel.recent_sequences.contains(sequence)) {
            channel.out_of_order_packets.fetch_add(1);
            return false;
        }

        const std::uint32_t forward = sequence - channel.highest_sequence;
        if (forward != 0 && forward < 0x80000000U) {
            if (forward > 1) {
                channel.lost_packets.fetch_add(static_cast<std::uint64_t>(forward - 1));
                const std::uint32_t tracked = std::min<std::uint32_t>(
                    forward - 1, static_cast<std::uint32_t>(sequence_horizon));
                for (std::uint32_t offset = 1; offset <= tracked; ++offset)
                    channel.missing_sequences.insert(sequence - offset);
            }
            channel.highest_sequence = sequence;
            remember_sequence(channel, sequence);
            return true;
        }

        channel.out_of_order_packets.fetch_add(1);
        if (channel.missing_sequences.erase(sequence) != 0) {
            const auto current = channel.lost_packets.load();
            if (current > 0) channel.lost_packets.fetch_sub(1);
            remember_sequence(channel, sequence);
            return true;
        }
        return false;
    }

    static bool metadata_matches(
        const WindowAssembly& assembly,
        const uevt::Packet& packet) {
        return assembly.sensor_width == packet.sensor_width &&
            assembly.sensor_height == packet.sensor_height &&
            assembly.window_start_us == packet.window_start_us &&
            assembly.window_end_us == packet.window_end_us;
    }

    static bool assembly_complete(const WindowAssembly& assembly) {
        return assembly.event_fragment_count != 0 &&
            assembly.box_fragment_count != 0 &&
            assembly.received_event_fragments == assembly.event_fragment_count &&
            assembly.received_box_fragments == assembly.box_fragment_count;
    }

    void process_packet(ChannelState& channel, uevt::Packet packet) {
        if (packet.message_type == uevt::MessageType::status) return;
        const AssemblyKey key{packet.window_id, packet.update_id, packet.flags};
        if (channel.completed.contains(key)) return;
        if (!channel.assemblies.contains(key) &&
            channel.assemblies.size() >= maximum_pending_windows) {
            auto oldest = std::min_element(
                channel.assemblies.begin(), channel.assemblies.end(),
                [](const auto& left, const auto& right) {
                    return left.second.first_seen < right.second.first_seen;
                });
            if (oldest != channel.assemblies.end()) {
                channel.pending_payload_bytes -= oldest->second.payload_bytes;
                channel.assemblies.erase(oldest);
                channel.incomplete_windows.fetch_add(1);
            }
        }

        auto [iterator, inserted] = channel.assemblies.try_emplace(key);
        WindowAssembly& assembly = iterator->second;
        if (inserted) {
            assembly.sensor_width = packet.sensor_width;
            assembly.sensor_height = packet.sensor_height;
            assembly.window_start_us = packet.window_start_us;
            assembly.window_end_us = packet.window_end_us;
            assembly.first_seen = std::chrono::steady_clock::now();
        } else if (!metadata_matches(assembly, packet)) {
            channel.invalid_packets.fetch_add(1);
            channel.pending_payload_bytes -= assembly.payload_bytes;
            channel.assemblies.erase(iterator);
            set_error("UEVT V2 event and box fragments have conflicting metadata.");
            return;
        }

        const std::size_t added_bytes =
            packet.events.size() * sizeof(EventPoint) +
            packet.boxes.size() * sizeof(DetectionBox);
        if (added_bytes > maximum_pending_bytes ||
            channel.pending_payload_bytes > maximum_pending_bytes - added_bytes) {
            channel.pending_payload_bytes -= assembly.payload_bytes;
            channel.assemblies.erase(iterator);
            channel.incomplete_windows.fetch_add(1);
            set_error("UEVT V2 reassembly exceeded the 64 MiB per-camera limit.");
            return;
        }

        if (packet.message_type == uevt::MessageType::events) {
            if (assembly.event_fragment_count == 0) {
                assembly.event_fragment_count = packet.fragment_count;
                assembly.event_fragments.resize(packet.fragment_count);
                assembly.event_received.resize(packet.fragment_count, false);
            } else if (assembly.event_fragment_count != packet.fragment_count) {
                reject_assembly(channel, iterator,
                    "UEVT V2 event fragments disagree on fragment_count.");
                return;
            }
            if (assembly.event_received[packet.fragment_index]) {
                channel.out_of_order_packets.fetch_add(1);
                return;
            }
            assembly.event_fragments[packet.fragment_index] = std::move(packet.events);
            assembly.event_received[packet.fragment_index] = true;
            assembly.payload_bytes += added_bytes;
            channel.pending_payload_bytes += added_bytes;
            ++assembly.received_event_fragments;
        } else {
            if (assembly.box_fragment_count == 0) {
                assembly.box_fragment_count = packet.fragment_count;
                assembly.box_fragments.resize(packet.fragment_count);
                assembly.box_received.resize(packet.fragment_count, false);
            } else if (assembly.box_fragment_count != packet.fragment_count) {
                reject_assembly(channel, iterator,
                    "UEVT V2 box fragments disagree on fragment_count.");
                return;
            }
            if (assembly.box_received[packet.fragment_index]) {
                channel.out_of_order_packets.fetch_add(1);
                return;
            }
            assembly.box_fragments[packet.fragment_index] = std::move(packet.boxes);
            assembly.box_received[packet.fragment_index] = true;
            assembly.payload_bytes += added_bytes;
            channel.pending_payload_bytes += added_bytes;
            ++assembly.received_box_fragments;
        }

        if (!assembly_complete(assembly)) return;
        deliver_assembly(channel, key, assembly);
        channel.pending_payload_bytes -= assembly.payload_bytes;
        channel.assemblies.erase(iterator);
        channel.completed[key] = std::chrono::steady_clock::now();
    }

    using AssemblyIterator = std::unordered_map<
        AssemblyKey, WindowAssembly, AssemblyKeyHash>::iterator;

    void reject_assembly(
        ChannelState& channel,
        const AssemblyIterator iterator,
        const char* message) {
        channel.invalid_packets.fetch_add(1);
        channel.pending_payload_bytes -= iterator->second.payload_bytes;
        channel.assemblies.erase(iterator);
        set_error(message);
    }

    static void deliver_assembly(
        ChannelState& channel,
        const AssemblyKey& key,
        WindowAssembly& assembly) {
        std::size_t event_count = 0;
        for (const auto& fragment : assembly.event_fragments)
            event_count += fragment.size();
        std::vector<EventPoint> events;
        events.reserve(event_count);
        for (auto& fragment : assembly.event_fragments) {
            events.insert(
                events.end(),
                std::make_move_iterator(fragment.begin()),
                std::make_move_iterator(fragment.end()));
        }

        std::size_t box_count = 0;
        for (const auto& fragment : assembly.box_fragments) box_count += fragment.size();
        std::vector<DetectionBox> boxes;
        boxes.reserve(box_count);
        for (auto& fragment : assembly.box_fragments) {
            boxes.insert(
                boxes.end(),
                std::make_move_iterator(fragment.begin()),
                std::make_move_iterator(fragment.end()));
        }

        channel.total_events.fetch_add(events.size());
        const bool historical = (key.flags & 1U) != 0;
        const bool raw_source = key.flags == 2U;
        const bool filtered_display =
            channel.display_mode.load() == display_mode_filtered;

        std::vector<DetectionBox> display_boxes;
        if (filtered_display) {
            std::copy_if(
                boxes.begin(), boxes.end(), std::back_inserter(display_boxes),
                [](const DetectionBox& box) { return box.class_id == drone_class_id; });
        }

        if (!events.empty()) {
            if (filtered_display && raw_source) {
                std::vector<EventPoint> filtered_events;
                filtered_events.reserve(events.size());
                std::copy_if(
                    events.begin(), events.end(), std::back_inserter(filtered_events),
                    [&display_boxes](const EventPoint& event) {
                        return std::any_of(
                            display_boxes.begin(), display_boxes.end(),
                            [&event](const DetectionBox& box) {
                                return event.x >= box.x1 && event.x < box.x2 &&
                                    event.y >= box.y1 && event.y < box.y2;
                            });
                    });
                if (!filtered_events.empty())
                    channel.renderer.add_events(
                        filtered_events.data(),
                        filtered_events.data() + filtered_events.size());
            } else {
                channel.renderer.add_events(events.data(), events.data() + events.size());
            }
        }
        if (!historical) {
            channel.renderer.set_boxes(filtered_display ? display_boxes : boxes);
            channel.last_current_result = std::chrono::steady_clock::now();
            channel.boxes_visible = filtered_display
                ? !display_boxes.empty()
                : !boxes.empty();
        }
    }

    void purge_expired_windows() {
        const auto now = std::chrono::steady_clock::now();
        for (auto& channel : channels_) {
            if (!channel.enabled.load()) continue;
            std::lock_guard protocol_lock(channel.protocol_mutex);
            purge_expired_windows_locked(channel, now);
        }
    }

    static void purge_expired_windows_locked(
        ChannelState& channel,
        const std::chrono::steady_clock::time_point now) {
        for (auto iterator = channel.assemblies.begin();
             iterator != channel.assemblies.end();) {
            if (now - iterator->second.first_seen >= incomplete_window_timeout) {
                channel.incomplete_windows.fetch_add(1);
                channel.pending_payload_bytes -= iterator->second.payload_bytes;
                iterator = channel.assemblies.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = channel.completed.begin();
             iterator != channel.completed.end();) {
            if (now - iterator->second >= incomplete_window_timeout)
                iterator = channel.completed.erase(iterator);
            else
                ++iterator;
        }
    }

    void frame_loop() {
        auto next_frame = std::chrono::steady_clock::now();
        auto last_rate_time = next_frame;
        std::array<std::uint64_t, camera_count_per_group> last_rate_events{};
        while (running_.load()) {
            const auto interval = std::chrono::microseconds(
                frame_interval_microseconds_.load());
            next_frame += interval;
            std::this_thread::sleep_until(next_frame);
            if (!running_.load()) break;
            const auto now = std::chrono::steady_clock::now();
            for (auto& channel : channels_) {
                if (!channel.enabled.load()) continue;
                {
                    std::lock_guard protocol_lock(channel.protocol_mutex);
                    if (channel.boxes_visible &&
                        now - channel.last_current_result > stale_box_timeout) {
                        channel.renderer.clear_boxes();
                        channel.boxes_visible = false;
                    }
                }
                channel.renderer.publish_frame();
            }

            const double elapsed =
                std::chrono::duration<double>(now - last_rate_time).count();
            if (elapsed >= 1.0) {
                for (int index = 0; index < camera_count_per_group; ++index) {
                    ChannelState& channel = channels_[index];
                    const std::uint64_t current = channel.total_events.load();
                    channel.events_per_second.store(
                        static_cast<double>(current - last_rate_events[index]) / elapsed);
                    last_rate_events[index] = current;
                }
                last_rate_time = now;
            }
            if (next_frame + interval < now) next_frame = now;
        }
    }

    void set_error(std::string message) {
        std::lock_guard error_lock(error_mutex_);
        last_error_ = std::move(message);
    }

    mutable std::mutex lifecycle_mutex_;
    SOCKET socket_ = INVALID_SOCKET;
    std::thread receive_thread_;
    std::thread frame_thread_;
    std::atomic<bool> running_ = false;
    std::atomic<int> frame_interval_microseconds_ =
        default_frame_interval_microseconds;
    std::array<ChannelState, camera_count_per_group> channels_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

class EventCore final {
public:
    EventCore() {
        for (auto& group : groups_) group = std::make_unique<UdpGroupSession>();
        update_ports();
    }

    int initialize() {
        std::lock_guard state_lock(state_mutex_);
        if (initialized_) return 1;
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            set_error("WSAStartup failed. Error: " + std::to_string(result));
            return 0;
        }
        initialized_ = true;
        set_error("");
        return 1;
    }

    void shutdown() {
        stop_all();
        std::lock_guard state_lock(state_mutex_);
        if (initialized_) {
            WSACleanup();
            initialized_ = false;
        }
    }

    int set_network_settings(const char* bind_address, const int base_port) {
        if (bind_address == nullptr || bind_address[0] == '\0') {
            set_error("Bind address cannot be empty.");
            return 0;
        }
        if (base_port <= 0 || base_port + group_count - 1 > 65535) {
            set_error("UDP base port must leave room for 10 consecutive group ports.");
            return 0;
        }
        for (const auto& group : groups_) {
            if (group->is_running()) {
                set_error("Stop all UDP groups before changing network settings.");
                return 0;
            }
        }

        IN_ADDR address{};
        if (InetPtonA(AF_INET, bind_address, &address) != 1) {
            set_error("Bind address must be an IPv4 literal, for example 0.0.0.0.");
            return 0;
        }
        bind_address_ = bind_address;
        base_port_ = base_port;
        update_ports();
        set_error("");
        return 1;
    }

    int set_display_settings(
        const int width,
        const int height,
        const int frames_per_second) {
        if (!valid_display_settings(width, height, frames_per_second)) {
            set_error("Unsupported display resolution or frame rate.");
            return 0;
        }
        for (auto& group : groups_)
            group->configure_display(width, height, frames_per_second);
        display_width_ = width;
        display_height_ = height;
        frames_per_second_ = frames_per_second;
        set_error("");
        return 1;
    }

    int set_slot_display_mode(const int slot, const int mode) {
        if (!valid_slot(slot)) {
            set_error("UDP slot index is outside the valid range.");
            return 0;
        }
        if (mode != display_mode_raw && mode != display_mode_filtered) {
            set_error("Display mode must be 0 (raw) or 1 (filtered).");
            return 0;
        }
        const int result = groups_[slot / camera_count_per_group]->set_display_mode(
            slot % camera_count_per_group, mode);
        if (result == 0) {
            set_error("Failed to change the UDP slot display mode.");
            return 0;
        }
        set_error("");
        return 1;
    }

    int start_slot(const int slot) {
        if (!valid_slot(slot)) {
            set_error("UDP slot index is outside the valid range.");
            return 0;
        }
        {
            std::lock_guard state_lock(state_mutex_);
            if (!initialized_) {
                set_error("UDP core has not been initialized.");
                return 0;
            }
        }
        const int group_index = slot / camera_count_per_group;
        const int camera_index = slot % camera_count_per_group;
        groups_[group_index]->configure_display(
            display_width_, display_height_, frames_per_second_);
        const int result = groups_[group_index]->start_channel(
            camera_index, bind_address_, base_port_ + group_index);
        if (result == 0) {
            char message[512]{};
            groups_[group_index]->copy_error(message, sizeof(message));
            set_error(message);
        }
        return result;
    }

    void stop_slot(const int slot) {
        if (!valid_slot(slot)) return;
        groups_[slot / camera_count_per_group]->stop_channel(
            slot % camera_count_per_group);
    }

    void stop_all() {
        for (auto& group : groups_) group->stop_all();
    }

    int get_status(const int slot, EventCoreStatus* status) const {
        return valid_slot(slot)
            ? groups_[slot / camera_count_per_group]->get_status(
                slot % camera_count_per_group, status)
            : 0;
    }

    int copy_frame(
        const int slot,
        void* destination,
        const int capacity,
        const int stride,
        std::uint64_t* frame_id) const {
        return valid_slot(slot)
            ? groups_[slot / camera_count_per_group]->copy_frame(
                slot % camera_count_per_group,
                destination, capacity, stride, frame_id)
            : 0;
    }

    int copy_slot_error(const int slot, char* buffer, const int capacity) const {
        return valid_slot(slot)
            ? groups_[slot / camera_count_per_group]->copy_error(buffer, capacity)
            : copy_string("Invalid UDP slot.", buffer, capacity);
    }

    int copy_last_error(char* buffer, const int capacity) const {
        std::lock_guard error_lock(error_mutex_);
        return copy_string(last_error_, buffer, capacity);
    }

private:
    static bool valid_slot(const int slot) {
        return slot >= 0 && slot < EVENTCORE_SLOT_COUNT;
    }

    void update_ports() {
        for (int group = 0; group < group_count; ++group)
            groups_[group]->set_listen_port(base_port_ + group);
    }

    void set_error(std::string message) {
        std::lock_guard error_lock(error_mutex_);
        last_error_ = std::move(message);
    }

    std::array<std::unique_ptr<UdpGroupSession>, group_count> groups_;
    std::string bind_address_ = "0.0.0.0";
    int base_port_ = default_base_port;
    int display_width_ = EventFrameRenderer::default_output_width;
    int display_height_ = EventFrameRenderer::default_output_height;
    int frames_per_second_ = default_frames_per_second;
    mutable std::mutex state_mutex_;
    bool initialized_ = false;
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

EventCore core;

}

extern "C" {

std::int32_t EVENTCORE_CALL ec_initialize() { return core.initialize(); }
void EVENTCORE_CALL ec_shutdown() { core.shutdown(); }

std::int32_t EVENTCORE_CALL ec_set_network_settings(
    const char* bind_address,
    const std::int32_t base_port) {
    return core.set_network_settings(bind_address, base_port);
}

std::int32_t EVENTCORE_CALL ec_set_display_settings(
    const std::int32_t width,
    const std::int32_t height,
    const std::int32_t frames_per_second) {
    return core.set_display_settings(width, height, frames_per_second);
}

std::int32_t EVENTCORE_CALL ec_set_slot_display_mode(
    const std::int32_t slot,
    const std::int32_t mode) {
    return core.set_slot_display_mode(slot, mode);
}

std::int32_t EVENTCORE_CALL ec_start_udp_slot(const std::int32_t slot) {
    return core.start_slot(slot);
}
void EVENTCORE_CALL ec_stop_udp_slot(const std::int32_t slot) {
    core.stop_slot(slot);
}
void EVENTCORE_CALL ec_stop_all_udp_slots() { core.stop_all(); }

std::int32_t EVENTCORE_CALL ec_get_slot_status(
    const std::int32_t slot,
    EventCoreStatus* status) {
    return core.get_status(slot, status);
}

std::int32_t EVENTCORE_CALL ec_copy_slot_frame(
    const std::int32_t slot,
    void* destination,
    const std::int32_t destination_capacity,
    const std::int32_t destination_stride,
    std::uint64_t* frame_id) {
    return core.copy_frame(
        slot, destination, destination_capacity, destination_stride, frame_id);
}

std::int32_t EVENTCORE_CALL ec_get_slot_error(
    const std::int32_t slot,
    char* buffer,
    const std::int32_t buffer_capacity) {
    return core.copy_slot_error(slot, buffer, buffer_capacity);
}

std::int32_t EVENTCORE_CALL ec_get_last_error(
    char* buffer,
    const std::int32_t buffer_capacity) {
    return core.copy_last_error(buffer, buffer_capacity);
}

}
