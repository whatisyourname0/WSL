# Keep each existing suite in its own process, especially the fake HDV adapter
# which deliberately has different symbols/definitions from the product library.
set(virtio_sound_suites
    wsl_virtio_sound_rx_doorbell_lifecycle_tests
    wsl_virtio_sound_event_scheduler_tests
    wsl_virtio_sound_scheduler_simulation_tests
    wsl_virtio_sound_host_timing_diagnostics_tests
    wsl_virtio_sound_diagnostics_tests
    wsl_virtio_sound_tests
    wsl_virtio_sound_pcm_source_lifecycle_tests
    wsl_virtio_sound_pcm_ring_tests
    wsl_virtio_sound_pcm_ring_core_integration_tests
    wsl_virtio_sound_capture_wake_deferral_tests
    wsl_virtio_sound_restricted_user_token_tests
    wsl_virtio_sound_worker_pipe_security_tests
    wsl_virtio_sound_hdv_lifecycle_tests
    wsl_virtio_sound_audio_ipc_tests
    wsl_virtio_sound_audio_pipe_transport_tests
    wsl_virtio_sound_async_pipe_tests
    wsl_virtio_sound_audio_worker_tests
    wsl_virtio_sound_wasapi_initial_packet_policy_tests
    wsl_virtio_sound_wasapi_capture_timing_tests
    wsl_virtio_sound_wasapi_open_policy_tests
    wsl_virtio_sound_audio_worker_session_tests)

set(taef_source ${CMAKE_CURRENT_SOURCE_DIR}/VirtioSoundUnitTests.cpp)
# Keep the adapter independent of the common PCH and Unity batches, so the same
# source also builds as a strictly offline TAEF module for fork CI.
set_source_files_properties(${taef_source} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON SKIP_UNITY_BUILD_INCLUSION ON)

if(TARGET wsltests)
    set_source_files_properties(${taef_source} TARGET_DIRECTORY wsltests PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON SKIP_UNITY_BUILD_INCLUSION ON)
    target_sources(wsltests PRIVATE ${taef_source})
    add_dependencies(wsltests ${virtio_sound_suites})
endif()

set(WSL_VIRTIO_SOUND_TAEF_ROOT "${TAEF_SOURCE_DIR}" CACHE PATH "Microsoft.Taef NuGet package directory")
if(NOT EXISTS "${WSL_VIRTIO_SOUND_TAEF_ROOT}/build/Include/WexTestClass.h")
    message(FATAL_ERROR "Set WSL_VIRTIO_SOUND_TAEF_ROOT to the repository's Microsoft.Taef package")
endif()
if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$")
    set(taef_arch arm64)
else()
    set(taef_arch x64)
endif()

add_library(wsl_virtio_sound_taef_tests SHARED ${taef_source})
target_compile_features(wsl_virtio_sound_taef_tests PRIVATE cxx_std_17)
target_compile_definitions(wsl_virtio_sound_taef_tests PRIVATE
    INLINE_TEST_METHOD_MARKUP WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_include_directories(wsl_virtio_sound_taef_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    "${WSL_VIRTIO_SOUND_TAEF_ROOT}/build/Include")
target_link_directories(wsl_virtio_sound_taef_tests PRIVATE
    "${WSL_VIRTIO_SOUND_TAEF_ROOT}/build/Library/${taef_arch}")
target_link_libraries(wsl_virtio_sound_taef_tests PRIVATE
    wsl_virtio_sound_windows_support TE.Common.lib Wex.Common.lib Wex.Logger.lib)
set_target_properties(wsl_virtio_sound_taef_tests PROPERTIES UNITY_BUILD OFF)
add_dependencies(wsl_virtio_sound_taef_tests ${virtio_sound_suites})

# Full builds already place these files beside wsltests.dll. Standalone CMake
# uses several output directories, so collect the exact suite targets beside
# the adapter, without requiring PATH or a particular working directory.
foreach(suite IN LISTS virtio_sound_suites)
    add_custom_command(TARGET wsl_virtio_sound_taef_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${suite}> $<TARGET_FILE_DIR:wsl_virtio_sound_taef_tests>
        VERBATIM)
endforeach()
