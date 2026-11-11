// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace wsl::virtio_sound::audio_worker {

// Requested endpoint reserve. Shared event-driven WASAPI support for a
// nonzero duration must be verified on the target endpoint; the resulting
// buffer size remains the source of truth for the notification watchdog.
inline constexpr std::chrono::milliseconds c_captureBufferReserve{100};

// Bounds time spent waiting after GetNextPacketSize reports no data. A signaled
// capture event still wakes the caller immediately. The policy uses half the
// reported endpoint-buffer duration, capped at 50 ms; unknown endpoint details
// use a 10 ms fallback, which cannot guarantee a half-buffer bound when the
// actual endpoint size is unknown. `requested` can impose a shorter timeout,
// while zero or negative values request an immediate retry.
inline std::chrono::milliseconds CaptureNotificationWaitTimeout(
    std::chrono::milliseconds requested, std::uint32_t bufferFrames, std::uint32_t sampleRate) noexcept
{
    if (requested <= std::chrono::milliseconds::zero())
    {
        return std::chrono::milliseconds::zero();
    }

    constexpr std::uint64_t c_millisecondsPerSecond = 1000U;
    constexpr std::uint64_t c_halfMillisecondsPerSecond = c_millisecondsPerSecond / 2U;
    constexpr std::int64_t c_minimumTimeoutMilliseconds = 1;
    constexpr std::int64_t c_maximumTimeoutMilliseconds = 50;
    constexpr std::int64_t c_unknownEndpointTimeoutMilliseconds = 10;

    std::int64_t endpointLimitMilliseconds = c_unknownEndpointTimeoutMilliseconds;
    if (bufferFrames != 0U && sampleRate != 0U)
    {
        // uint32 frames * 500 fits comfortably in uint64. Integer division
        // intentionally floors the half-buffer duration in milliseconds.
        const std::uint64_t halfBufferMilliseconds =
            (static_cast<std::uint64_t>(bufferFrames) * c_halfMillisecondsPerSecond) / sampleRate;
        endpointLimitMilliseconds = static_cast<std::int64_t>((std::clamp)(
            halfBufferMilliseconds,
            static_cast<std::uint64_t>(c_minimumTimeoutMilliseconds),
            static_cast<std::uint64_t>(c_maximumTimeoutMilliseconds)));
    }

    const auto boundedRequest = (std::min)(requested.count(), endpointLimitMilliseconds);
    return std::chrono::milliseconds{boundedRequest};
}

} // namespace wsl::virtio_sound::audio_worker
