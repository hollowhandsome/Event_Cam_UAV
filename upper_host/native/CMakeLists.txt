cmake_minimum_required(VERSION 3.20)
project(EventCoreUdp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

set(EVENTCORE_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin" CACHE PATH
    "Directory that receives EventCore.dll")

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc)

add_library(EventCore SHARED
    include/EventCoreApi.h
    src/EventTypes.h
    src/UevtProtocol.h
    src/UevtProtocol.cpp
    src/EventFrameRenderer.h
    src/EventFrameRenderer.cpp
    src/EventCore.cpp
)

target_compile_definitions(EventCore PRIVATE
    EVENTCORE_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_include_directories(EventCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(EventCore PRIVATE
    opencv_core
    opencv_imgproc
    ws2_32
)

add_custom_command(TARGET EventCore POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        $<TARGET_RUNTIME_DLLS:EventCore>
        "$<TARGET_FILE_DIR:EventCore>"
    COMMAND_EXPAND_LISTS
    COMMENT "Copying OpenCV runtime libraries"
)

set_target_properties(EventCore PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${EVENTCORE_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${EVENTCORE_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${EVENTCORE_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
)

enable_testing()
add_executable(UevtProtocolTests
    tests/UevtProtocolTests.cpp
    src/UevtProtocol.cpp
)
target_include_directories(UevtProtocolTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME UevtProtocolTests COMMAND UevtProtocolTests)

set(UEVT_V2_DELIVERY_SAMPLES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../event_cam_studio_developer_thor_arm64_v2_20260924/integration/samples/raw/packet_001.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../event_cam_studio_developer_thor_arm64_v2_20260924/integration/samples/raw/packet_002.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../event_cam_studio_developer_thor_arm64_v2_20260924/integration/samples/filtered/packet_001.bin"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../event_cam_studio_developer_thor_arm64_v2_20260924/integration/samples/raw/packet_001.bin")
    add_test(
        NAME UevtV2DeliverySamples
        COMMAND UevtProtocolTests ${UEVT_V2_DELIVERY_SAMPLES}
    )
endif()

if(MSVC)
    target_compile_options(EventCore PRIVATE /W4 /permissive- /utf-8 /external:W0)
    target_compile_options(UevtProtocolTests PRIVATE /W4 /permissive- /utf-8)
endif()
