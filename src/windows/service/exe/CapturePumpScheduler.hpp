// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>
#include <limits>

namespace wsl::virtio_sound::process_host::detail {

// Tracks absolute pump deadlines. A delayed pump is serviced once; deadlines
// that passed while it was running are skipped instead of causing a catch-up
// burst. Times are monotonic nanoseconds relative to the service loop start.
class CapturePumpScheduler final
{
public:
    static constexpr std::uint64_t c_nanosecondsPerMillisecond = 1'000'000;
    static constexpr std::uint32_t c_maxFiniteWaitMilliseconds = (std::numeric_limits<std::uint32_t>::max)() - 1;

    CapturePumpScheduler(std::uint64_t intervalNanoseconds, std::uint64_t nowNanoseconds) noexcept
    {
        Reset(intervalNanoseconds, nowNanoseconds);
    }

    void Reset(std::uint64_t intervalNanoseconds, std::uint64_t nowNanoseconds) noexcept
    {
        m_intervalNanoseconds = (intervalNanoseconds == 0) ? 1 : intervalNanoseconds;
        m_nextDeadlineNanoseconds = AddDeadline(nowNanoseconds, m_intervalNanoseconds, m_deadlineSaturated);
        m_scheduledTicks = 0;
        m_lateTicks = 0;
        m_skippedDeadlines = 0;
    }

    // A capture state transition may change the target cadence. Starting a
    // fresh phase at that transition avoids an immediate stale or catch-up tick.
    void SetInterval(std::uint64_t intervalNanoseconds, std::uint64_t nowNanoseconds) noexcept
    {
        intervalNanoseconds = (intervalNanoseconds == 0) ? 1 : intervalNanoseconds;
        if (intervalNanoseconds == m_intervalNanoseconds)
        {
            return;
        }

        m_intervalNanoseconds = intervalNanoseconds;
        m_nextDeadlineNanoseconds = AddDeadline(nowNanoseconds, m_intervalNanoseconds, m_deadlineSaturated);
    }

    std::uint32_t WaitTimeoutMilliseconds(std::uint64_t nowNanoseconds) const noexcept
    {
        if (nowNanoseconds >= m_nextDeadlineNanoseconds)
        {
            // At the saturated end of the clock's range there is no representable
            // future deadline. Keep waiting for stop/parent handles without a
            // zero-timeout spin.
            return m_deadlineSaturated ? c_maxFiniteWaitMilliseconds : 0;
        }

        const std::uint64_t remaining = m_nextDeadlineNanoseconds - nowNanoseconds;
        const std::uint64_t milliseconds = remaining / c_nanosecondsPerMillisecond +
                                           ((remaining % c_nanosecondsPerMillisecond) != 0 ? 1 : 0);
        return (milliseconds > c_maxFiniteWaitMilliseconds) ? c_maxFiniteWaitMilliseconds : static_cast<std::uint32_t>(milliseconds);
    }

    // Call once immediately before servicing a scheduled pump. This consumes
    // the due tick and advances the phase past any deadlines already missed.
    void PumpStarted(std::uint64_t nowNanoseconds) noexcept
    {
        Increment(m_scheduledTicks, 1);
        if (nowNanoseconds < m_nextDeadlineNanoseconds)
        {
            return;
        }

        if (nowNanoseconds > m_nextDeadlineNanoseconds)
        {
            Increment(m_lateTicks, 1);
        }

        const std::uint64_t lateBy = nowNanoseconds - m_nextDeadlineNanoseconds;
        const std::uint64_t missedBeforeStart = lateBy / m_intervalNanoseconds;
        Increment(m_skippedDeadlines, missedBeforeStart);
        const std::uint64_t untilNext = m_intervalNanoseconds - (lateBy % m_intervalNanoseconds);
        SetNextDeadlineAfter(nowNanoseconds, untilNext);
    }

    // Call after the pump work completes. If processing crossed later deadlines,
    // move directly to the next future slot while retaining the original phase.
    void PumpCompleted(std::uint64_t nowNanoseconds) noexcept
    {
        if (m_deadlineSaturated || nowNanoseconds < m_nextDeadlineNanoseconds)
        {
            return;
        }

        const std::uint64_t lateBy = nowNanoseconds - m_nextDeadlineNanoseconds;
        std::uint64_t missedDuringPump = lateBy / m_intervalNanoseconds;
        if (missedDuringPump != (std::numeric_limits<std::uint64_t>::max)())
        {
            ++missedDuringPump;
        }
        Increment(m_skippedDeadlines, missedDuringPump);
        const std::uint64_t untilNext = m_intervalNanoseconds - (lateBy % m_intervalNanoseconds);
        SetNextDeadlineAfter(nowNanoseconds, untilNext);
    }

    std::uint64_t ScheduledTicks() const noexcept { return m_scheduledTicks; }
    std::uint64_t LateTicks() const noexcept { return m_lateTicks; }
    std::uint64_t SkippedDeadlines() const noexcept { return m_skippedDeadlines; }
    std::uint64_t NextDeadlineNanoseconds() const noexcept { return m_nextDeadlineNanoseconds; }

private:
    static void Increment(std::uint64_t& value, std::uint64_t amount) noexcept
    {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        value = amount > maximum - value ? maximum : value + amount;
    }

    static std::uint64_t AddDeadline(std::uint64_t now, std::uint64_t delay, bool& saturated) noexcept
    {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (delay >= maximum - now)
        {
            saturated = true;
            return maximum;
        }

        saturated = false;
        return now + delay;
    }

    void SetNextDeadlineAfter(std::uint64_t now, std::uint64_t delay) noexcept
    {
        m_nextDeadlineNanoseconds = AddDeadline(now, delay, m_deadlineSaturated);
    }

    std::uint64_t m_intervalNanoseconds{};
    std::uint64_t m_nextDeadlineNanoseconds{};
    std::uint64_t m_scheduledTicks{};
    std::uint64_t m_lateTicks{};
    std::uint64_t m_skippedDeadlines{};
    bool m_deadlineSaturated{};
};

} // namespace wsl::virtio_sound::process_host::detail
