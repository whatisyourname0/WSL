// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wsl::virtio_sound::process_host::detail {

struct CaptureHostTimingEvent final
{
    std::uint64_t m_sequence{};
    std::uint64_t m_waitStartedAtNanoseconds{};
    std::uint64_t m_requestedWaitNanoseconds{};
    std::uint64_t m_waitReturnedAtNanoseconds{};
    std::uint64_t m_actualWaitNanoseconds{};
    std::uint64_t m_serviceStartedAtNanoseconds{};
    std::uint64_t m_serviceDurationNanoseconds{};
    std::uint64_t m_generation{};
    std::uint32_t m_waitResult{};
    std::int32_t m_frames{};
    std::uint8_t m_waitReason{};
    std::uint8_t m_flags{};
};

// Keeps only the most recent anomalous host-pump intervals. Recording is
// allocation-free because it runs on the capture host's service thread.
class CaptureHostTimingRing final
{
public:
    static constexpr std::size_t c_capacity = 32;
    static constexpr std::uint64_t c_slowServiceThresholdNanoseconds = 20'000'000;
    static constexpr std::uint64_t c_deadlineOverrunToleranceNanoseconds = 1'000'000;
    static constexpr std::uint8_t c_deadlineOverrun = 0x1;
    static constexpr std::uint8_t c_slowService = 0x2;

    bool RecordIfSlow(
        std::uint64_t waitStartedAtNanoseconds,
        std::uint32_t requestedWaitMilliseconds,
        std::uint64_t waitReturnedAtNanoseconds,
        std::uint32_t waitResult,
        std::uint64_t serviceStartedAtNanoseconds,
        std::uint64_t serviceDurationNanoseconds,
        std::uint8_t waitReason,
        std::int32_t frames,
        std::uint64_t generation) noexcept
    {
        constexpr std::uint32_t c_infiniteWait = (std::numeric_limits<std::uint32_t>::max)();
        const bool infiniteWait = requestedWaitMilliseconds == c_infiniteWait;
        const auto requestedWaitNanoseconds = infiniteWait ? (std::numeric_limits<std::uint64_t>::max)() :
            static_cast<std::uint64_t>(requestedWaitMilliseconds) * 1'000'000;
        const auto actualWaitNanoseconds = waitReturnedAtNanoseconds >= waitStartedAtNanoseconds ?
            waitReturnedAtNanoseconds - waitStartedAtNanoseconds :
            0;
        const bool deadlineOverrun = !infiniteWait && actualWaitNanoseconds > requestedWaitNanoseconds &&
            actualWaitNanoseconds - requestedWaitNanoseconds > c_deadlineOverrunToleranceNanoseconds;
        const bool slowService = serviceDurationNanoseconds > c_slowServiceThresholdNanoseconds;
        if (!deadlineOverrun && !slowService)
        {
            return false;
        }

        std::uint8_t flags{};
        if (deadlineOverrun)
        {
            flags |= c_deadlineOverrun;
        }
        if (slowService)
        {
            flags |= c_slowService;
        }
        m_events[m_next] = {
            m_nextSequence++,
            waitStartedAtNanoseconds,
            requestedWaitNanoseconds,
            waitReturnedAtNanoseconds,
            actualWaitNanoseconds,
            serviceStartedAtNanoseconds,
            serviceDurationNanoseconds,
            generation,
            waitResult,
            frames,
            waitReason,
            flags};
        m_next = (m_next + 1) % c_capacity;
        if (m_count < c_capacity)
        {
            ++m_count;
        }
        return true;
    }

    std::size_t Count() const noexcept
    {
        return m_count;
    }

    std::uint64_t TotalRecorded() const noexcept
    {
        return m_nextSequence;
    }

    const CaptureHostTimingEvent* Oldest(std::size_t index) const noexcept
    {
        if (index >= m_count)
        {
            return nullptr;
        }

        const auto oldest = (m_next + c_capacity - m_count) % c_capacity;
        return &m_events[(oldest + index) % c_capacity];
    }

private:
    std::array<CaptureHostTimingEvent, c_capacity> m_events{};
    std::size_t m_next{};
    std::size_t m_count{};
    std::uint64_t m_nextSequence{};
};
} // namespace wsl::virtio_sound::process_host::detail
