#pragma once

#include <cstdint>

#ifdef _WIN32
#ifdef EVENTCORE_EXPORTS
#define EVENTCORE_API __declspec(dllexport)
#else
#define EVENTCORE_API __declspec(dllimport)
#endif
#define EVENTCORE_CALL __cdecl
#else
#define EVENTCORE_API
#define EVENTCORE_CALL
#endif

extern "C" {

constexpr std::int32_t EVENTCORE_SLOT_COUNT = 20;

struct EventCoreStatus {
    std::int32_t state;
    std::int32_t width;
    std::int32_t height;
    std::int32_t listen_port;
    std::uint64_t total_events;
    std::uint64_t received_packets;
    std::uint64_t lost_packets;
    std::uint64_t out_of_order_packets;
    std::uint64_t invalid_packets;
    std::uint64_t incomplete_windows;
    double events_per_second;
    std::uint64_t frame_id;
};

EVENTCORE_API std::int32_t EVENTCORE_CALL ec_initialize();
EVENTCORE_API void EVENTCORE_CALL ec_shutdown();

EVENTCORE_API std::int32_t EVENTCORE_CALL ec_set_network_settings(
    const char* bind_address,
    std::int32_t base_port);
EVENTCORE_API std::int32_t EVENTCORE_CALL ec_set_display_settings(
    std::int32_t width,
    std::int32_t height,
    std::int32_t frames_per_second);
EVENTCORE_API std::int32_t EVENTCORE_CALL ec_set_slot_display_mode(
    std::int32_t slot,
    std::int32_t mode);

EVENTCORE_API std::int32_t EVENTCORE_CALL ec_start_udp_slot(std::int32_t slot);
EVENTCORE_API void EVENTCORE_CALL ec_stop_udp_slot(std::int32_t slot);
EVENTCORE_API void EVENTCORE_CALL ec_stop_all_udp_slots();

EVENTCORE_API std::int32_t EVENTCORE_CALL ec_get_slot_status(
    std::int32_t slot,
    EventCoreStatus* status);
EVENTCORE_API std::int32_t EVENTCORE_CALL ec_copy_slot_frame(
    std::int32_t slot,
    void* destination,
    std::int32_t destination_capacity,
    std::int32_t destination_stride,
    std::uint64_t* frame_id);

EVENTCORE_API std::int32_t EVENTCORE_CALL ec_get_slot_error(
    std::int32_t slot,
    char* buffer,
    std::int32_t buffer_capacity);
EVENTCORE_API std::int32_t EVENTCORE_CALL ec_get_last_error(
    char* buffer,
    std::int32_t buffer_capacity);

}
