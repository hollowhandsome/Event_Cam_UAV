cmake_minimum_required(VERSION 3.20)
project(EventCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

set(DVSENSE_SDK_ROOT "C:/Program Files (x86)/DvsenseDriver" CACHE PATH
    "DVSense SDK installation directory")
set(EVENTCORE_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin" CACHE PATH
    "Directory that receives EventCore.dll")

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc)

foreach(required_file
        "${DVSENSE_SDK_ROOT}/include/DvsenseDriver/camera/DvsCameraManager.hpp"
        "${DVSENSE_SDK_ROOT}/lib/DvsenseBase.lib"
        "${DVSENSE_SDK_ROOT}/lib/DvsenseHal.lib"
        "${DVSENSE_SDK_ROOT}/lib/DvsenseDriver.lib")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Required DVSense SDK file is missing: ${required_file}")
    endif()
endforeach()

add_library(EventCore SHARED
    include/EventCoreApi.h
    src/EventFrameRenderer.h
    src/EventFrameRenderer.cpp
    src/EventCore.cpp
)

target_compile_definitions(EventCore PRIVATE
    EVENTCORE_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_include_directories(EventCore
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(EventCore SYSTEM PRIVATE "${DVSENSE_SDK_ROOT}/include")
target_link_libraries(EventCore PRIVATE
    "${DVSENSE_SDK_ROOT}/lib/DvsenseDriver.lib"
    "${DVSENSE_SDK_ROOT}/lib/DvsenseHal.lib"
    "${DVSENSE_SDK_ROOT}/lib/DvsenseBase.lib"
    opencv_core
    opencv_imgproc
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

if(MSVC)
    target_compile_options(EventCore PRIVATE /W4 /permissive- /utf-8 /external:W0)
endif()
