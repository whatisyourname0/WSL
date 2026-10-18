cmake_minimum_required(VERSION 3.20)
project(virtio_sound_core_benchmark LANGUAGES CXX)

# Keep the harness fixed while selecting baseline or candidate production code.
set(WSL_VIRTIO_SOUND_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "virtiosound source tree to benchmark")
add_executable(virtio_sound_core_benchmark
    ../tests/VirtioSoundDevicePerfBenchmark.cpp
    ${WSL_VIRTIO_SOUND_SOURCE_DIR}/src/VirtioSoundDevice.cpp)
target_compile_features(virtio_sound_core_benchmark PRIVATE cxx_std_17)
target_include_directories(virtio_sound_core_benchmark PRIVATE
    ${WSL_VIRTIO_SOUND_SOURCE_DIR}/include)
if(WIN32)
    set(WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR "" CACHE PATH "Directory containing wil/resource.h")
    if(NOT EXISTS "${WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR}/wil/resource.h")
        message(FATAL_ERROR "Set WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR to existing WIL headers")
    endif()
    target_include_directories(virtio_sound_core_benchmark PRIVATE
        ${WSL_VIRTIO_SOUND_SOURCE_DIR} ${WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR})
    target_link_libraries(virtio_sound_core_benchmark PRIVATE advapi32)
endif()
if(MSVC)
    target_compile_options(virtio_sound_core_benchmark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
