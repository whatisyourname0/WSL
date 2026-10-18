// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <cstdint>
#include <limits>
#include "VirtioSoundDevice.hpp"

namespace wsl::virtio_sound::process_host::detail {

// A completion-rate guard, not a polling cadence. Hardware capture has no
// timer while waiting for data, RX, or readiness. Deferred runnable work arms
// one deadline. Synthetic sources also need a deadline to produce samples.
class CaptureEventScheduler final
{
public:
    bool CanTransfer(std::uint64_t now, std::uint64_t generation) noexcept
    {
        if (generation != m_generation)
        {
            m_generation = generation;
            m_nextAllowed = now;
            m_armed = false;
        }
        return now >= m_nextAllowed;
    }

    void OnService(std::uint64_t startedAt, const native::CaptureServiceResult& result) noexcept
    {
        (void)CanTransfer(startedAt, result.m_generation);
        using Reason = native::CaptureWaitReason;
        if (result.m_waitReason == Reason::Idle || result.m_waitReason == Reason::Readiness || result.m_waitReason == Reason::Failed)
        {
            m_armed = false;
            return;
        }
        if (result.m_frames > 0)
        {
            const auto duration = Duration(static_cast<std::uint32_t>(result.m_frames), result.m_sampleRate);
            // Preserve the sample-clock phase across sub-period scheduling
            // jitter. Restarting from every actual wake accumulates rounding
            // error and eventually fills a source-clocked capture queue.
            // Rebase after a whole missed period instead of replaying an
            // unbounded backlog of deadlines after a host scheduling stall.
            const auto base = startedAt >= m_nextAllowed && startedAt - m_nextAllowed < duration ? m_nextAllowed : startedAt;
            m_nextAllowed = Add(base, duration);
        }
        if (!result.m_sourceClocked)
        {
            if (m_nextAllowed <= startedAt)
            {
                m_nextAllowed = Add(startedAt, Duration(result.m_periodFrames, result.m_sampleRate));
            }
            m_armed = true;
        }
        else
        {
            m_armed = result.m_moreSourceFrames &&
                (result.m_waitReason == Reason::Progress || result.m_waitReason == Reason::PacingDeadline);
        }
    }

    std::uint32_t WaitTimeoutMilliseconds(std::uint64_t now) const noexcept
    {
        constexpr auto infinite = (std::numeric_limits<std::uint32_t>::max)();
        if (!m_armed)
        {
            return infinite;
        }
        if (now >= m_nextAllowed)
        {
            return m_nextAllowed == (std::numeric_limits<std::uint64_t>::max)() ? infinite - 1 : 0;
        }
        const auto delta = m_nextAllowed - now;
        const auto milliseconds = delta / 1'000'000 + (delta % 1'000'000 != 0);
        return milliseconds >= infinite ? infinite - 1 : static_cast<std::uint32_t>(milliseconds);
    }

private:
    static std::uint64_t Duration(std::uint32_t frames, std::uint32_t rate) noexcept
    {
        if (!frames || !rate)
        {
            return 10'000'000;
        }
        return (static_cast<std::uint64_t>(frames) * 1'000'000'000 + rate - 1) / rate;
    }

    static std::uint64_t Add(std::uint64_t now, std::uint64_t duration) noexcept
    {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        return duration > maximum - now ? maximum : now + duration;
    }

    std::uint64_t m_generation{};
    std::uint64_t m_nextAllowed{};
    bool m_armed{};
};
} // namespace wsl::virtio_sound::process_host::detail
