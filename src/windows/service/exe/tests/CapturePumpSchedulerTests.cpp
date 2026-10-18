// Copyright (C) Microsoft Corporation. All rights reserved.

#include "../CapturePumpScheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using wsl::virtio_sound::process_host::detail::CapturePumpScheduler;

constexpr std::uint64_t c_nanosecondsPerMillisecond = 1'000'000;
constexpr std::uint64_t c_nanosecondsPerSecond = 1'000'000'000;
constexpr std::uint64_t c_sampleRate = 48'000;
constexpr std::uint64_t c_guestBufferFrames = 960;
constexpr std::uint64_t c_guestPeriodFrames = 480;
constexpr std::uint64_t c_maximumPumpBurstFrames = c_guestBufferFrames - c_guestPeriodFrames;
constexpr std::uint64_t c_simulationDurationNanoseconds = 10 * c_nanosecondsPerSecond;
constexpr std::uint64_t c_processingTimesNanoseconds[]{
    2 * c_nanosecondsPerMillisecond,
    2 * c_nanosecondsPerMillisecond,
    2 * c_nanosecondsPerMillisecond,
    7 * c_nanosecondsPerMillisecond,
    2 * c_nanosecondsPerMillisecond,
    2 * c_nanosecondsPerMillisecond,
    3 * c_nanosecondsPerMillisecond,
    1 * c_nanosecondsPerMillisecond,
};
constexpr std::uint64_t c_waitWakeJitterNanoseconds[]{
    0,
    1 * c_nanosecondsPerMillisecond,
    0,
    0,
    1 * c_nanosecondsPerMillisecond,
    0,
    0,
    0,
};

[[noreturn]] void Fail(const char* message)
{
    std::cerr << message << '\n';
    std::exit(1);
}

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        Fail(message);
    }
}

std::uint64_t ProducedFramesAt(std::uint64_t timeNanoseconds)
{
    // The simulation is deliberately short enough that this product is bounded.
    return timeNanoseconds * c_sampleRate / c_nanosecondsPerSecond;
}

class CaptureQueueSimulation final
{
public:
    void PumpAt(std::uint64_t timeNanoseconds)
    {
        ProduceUntil(timeNanoseconds);
        const std::uint64_t availableBudget = (std::min)(m_pendingFrames, c_maximumPumpBurstFrames);
        // The RX descriptor represents one guest period; a partial source buffer
        // stays queued until a whole period is available.
        const std::uint64_t completedFrames = (availableBudget / c_guestPeriodFrames) * c_guestPeriodFrames;
        m_pendingFrames -= completedFrames;
        m_deliveredFrames += completedFrames;
        m_maximumFramesPerPump = (std::max)(m_maximumFramesPerPump, completedFrames);
    }

    void RecordPostPump(std::uint64_t timeNanoseconds)
    {
        // Capture continues during pump processing, so include those arrivals in
        // the post-work backlog measured for this tick.
        ProduceUntil(timeNanoseconds);
        m_maximumPostPumpBacklog = (std::max)(m_maximumPostPumpBacklog, m_pendingFrames);
    }

    void FinishAt(std::uint64_t timeNanoseconds) { ProduceUntil(timeNanoseconds); }

    std::uint64_t TotalProducedFrames() const noexcept { return m_totalProducedFrames; }
    std::uint64_t DeliveredFrames() const noexcept { return m_deliveredFrames; }
    std::uint64_t PendingFrames() const noexcept { return m_pendingFrames; }
    std::uint64_t MaximumPostPumpBacklog() const noexcept { return m_maximumPostPumpBacklog; }
    std::uint64_t MaximumFramesPerPump() const noexcept { return m_maximumFramesPerPump; }

private:
    void ProduceUntil(std::uint64_t timeNanoseconds)
    {
        const std::uint64_t producedFrames = ProducedFramesAt(timeNanoseconds);
        m_pendingFrames += producedFrames - m_totalProducedFrames;
        m_totalProducedFrames = producedFrames;
    }

    std::uint64_t m_totalProducedFrames{};
    std::uint64_t m_deliveredFrames{};
    std::uint64_t m_pendingFrames{};
    std::uint64_t m_maximumPostPumpBacklog{};
    std::uint64_t m_maximumFramesPerPump{};
};

struct SimulationResult
{
    std::uint64_t pumpCount{};
    std::uint64_t lateTicks{};
    std::uint64_t skippedDeadlines{};
    std::uint64_t totalProducedFrames{};
    std::uint64_t deliveredFrames{};
    std::uint64_t pendingFrames{};
    std::uint64_t maximumPostPumpBacklog{};
    std::uint64_t maximumFramesPerPump{};
};

SimulationResult ResultFor(const CaptureQueueSimulation& source, std::uint64_t pumpCount, std::uint64_t lateTicks = 0, std::uint64_t skippedDeadlines = 0)
{
    return {
        pumpCount,
        lateTicks,
        skippedDeadlines,
        source.TotalProducedFrames(),
        source.DeliveredFrames(),
        source.PendingFrames(),
        source.MaximumPostPumpBacklog(),
        source.MaximumFramesPerPump(),
    };
}

SimulationResult SimulateRelativeTenMillisecondWait()
{
    // This models the old loop: wait 10 ms after each pump finishes, then
    // include the next pump's processing time before its work completes.
    CaptureQueueSimulation source;
    std::uint64_t now{};
    std::uint64_t tick{};
    while (now < c_simulationDurationNanoseconds)
    {
        const auto jitterIndex = tick % (sizeof(c_waitWakeJitterNanoseconds) / sizeof(c_waitWakeJitterNanoseconds[0]));
        const auto wakeTime = now + 10 * c_nanosecondsPerMillisecond + c_waitWakeJitterNanoseconds[jitterIndex];
        if (wakeTime >= c_simulationDurationNanoseconds)
        {
            break;
        }

        now = wakeTime;
        source.PumpAt(now);
        const auto processingIndex = tick % (sizeof(c_processingTimesNanoseconds) / sizeof(c_processingTimesNanoseconds[0]));
        now += c_processingTimesNanoseconds[processingIndex];
        source.RecordPostPump((std::min)(now, c_simulationDurationNanoseconds));
        ++tick;
    }

    source.FinishAt(c_simulationDurationNanoseconds);
    return ResultFor(source, tick);
}

SimulationResult SimulateAbsoluteActiveCadence()
{
    // This is a deterministic policy simulation, not a measurement of Windows
    // scheduling or WASAPI. Model 2 ms ordinary work, periodic 7 ms stalls, and
    // 1 ms wake jitter.
    // Capture frames arrive independently, while the guest only accepts whole
    // 480-frame periods and the host limits each pump to the 480-frame headroom.
    constexpr std::uint64_t c_intervalNanoseconds = 5 * c_nanosecondsPerMillisecond;
    CapturePumpScheduler scheduler(c_intervalNanoseconds, 0);
    CaptureQueueSimulation source;
    std::uint64_t now{};
    std::uint64_t tick{};

    while (now < c_simulationDurationNanoseconds)
    {
        const auto timeoutMilliseconds = scheduler.WaitTimeoutMilliseconds(now);
        const auto jitterIndex = tick % (sizeof(c_waitWakeJitterNanoseconds) / sizeof(c_waitWakeJitterNanoseconds[0]));
        const auto pumpStarted = now + static_cast<std::uint64_t>(timeoutMilliseconds) * c_nanosecondsPerMillisecond +
                                 c_waitWakeJitterNanoseconds[jitterIndex];
        if (pumpStarted >= c_simulationDurationNanoseconds)
        {
            break;
        }
        Require(pumpStarted >= scheduler.NextDeadlineNanoseconds(), "wait and wake jitter reach the scheduled deadline");

        now = pumpStarted;
        scheduler.PumpStarted(now);
        source.PumpAt(now);
        const auto processingIndex = tick % (sizeof(c_processingTimesNanoseconds) / sizeof(c_processingTimesNanoseconds[0]));
        now += c_processingTimesNanoseconds[processingIndex];
        source.RecordPostPump((std::min)(now, c_simulationDurationNanoseconds));
        scheduler.PumpCompleted(now);
        Require(scheduler.WaitTimeoutMilliseconds(now) != 0, "a completed pump waits for a future slot without catch-up");
        ++tick;
    }

    source.FinishAt(c_simulationDurationNanoseconds);
    return ResultFor(source, scheduler.ScheduledTicks(), scheduler.LateTicks(), scheduler.SkippedDeadlines());
}

void PrintResult(const char* label, const SimulationResult& result, bool deadlineStatsAvailable)
{
    std::cout << label << " pumps=" << result.pumpCount;
    if (deadlineStatsAvailable)
    {
        std::cout << " late_ticks=" << result.lateTicks << " skipped_deadlines=" << result.skippedDeadlines;
    }
    else
    {
        std::cout << " late_ticks=not_applicable skipped_deadlines=not_applicable";
    }
    std::cout << " max_postpump_backlog_frames=" << result.maximumPostPumpBacklog
              << " max_frames_per_pump=" << result.maximumFramesPerPump
              << " produced_frames=" << result.totalProducedFrames << " delivered_frames=" << result.deliveredFrames
              << " pending_frames=" << result.pendingFrames << '\n';
}

void RequireFrameConservation(const SimulationResult& result)
{
    Require(result.totalProducedFrames == result.deliveredFrames + result.pendingFrames, "produced frames equal delivered plus pending frames");
    Require(result.deliveredFrames % c_guestPeriodFrames == 0, "guest completion count contains only full periods");
    Require(result.maximumFramesPerPump <= c_maximumPumpBurstFrames, "a pump stays within the guest ring's one-period headroom");
}

void TestAbsoluteDeadlinesAndLongStall()
{
    constexpr std::uint64_t interval = 5 * c_nanosecondsPerMillisecond;
    CapturePumpScheduler scheduler(interval, 0);
    Require(scheduler.WaitTimeoutMilliseconds(0) == 5, "initial wait uses the first absolute deadline");

    scheduler.PumpStarted(5 * c_nanosecondsPerMillisecond);
    scheduler.PumpCompleted(7 * c_nanosecondsPerMillisecond);
    Require(scheduler.NextDeadlineNanoseconds() == 10 * c_nanosecondsPerMillisecond, "processing time does not shift the absolute phase");
    Require(scheduler.WaitTimeoutMilliseconds(7 * c_nanosecondsPerMillisecond) == 3, "next wait is measured to the absolute deadline");

    scheduler.PumpStarted(10 * c_nanosecondsPerMillisecond);
    scheduler.PumpCompleted(27 * c_nanosecondsPerMillisecond);
    Require(scheduler.NextDeadlineNanoseconds() == 30 * c_nanosecondsPerMillisecond, "long work skips missed deadlines without catch-up calls");
    Require(scheduler.SkippedDeadlines() == 3, "missed deadlines are counted with bounded arithmetic");
    Require(scheduler.ScheduledTicks() == 2, "one overdue period still produces one pump call");
    Require(scheduler.WaitTimeoutMilliseconds(27 * c_nanosecondsPerMillisecond) == 3, "a long stall waits for the next future slot");

    scheduler.SetInterval(10 * c_nanosecondsPerMillisecond, 27 * c_nanosecondsPerMillisecond);
    Require(scheduler.NextDeadlineNanoseconds() == 37 * c_nanosecondsPerMillisecond, "cadence transition starts a fresh phase");

    const auto nearMaximum = (std::numeric_limits<std::uint64_t>::max)() - interval;
    CapturePumpScheduler nearOverflow(interval, nearMaximum);
    Require(nearOverflow.NextDeadlineNanoseconds() == (std::numeric_limits<std::uint64_t>::max)(), "deadline addition saturates");
    nearOverflow.PumpStarted((std::numeric_limits<std::uint64_t>::max)());
    nearOverflow.PumpCompleted((std::numeric_limits<std::uint64_t>::max)());
    Require(
        nearOverflow.WaitTimeoutMilliseconds((std::numeric_limits<std::uint64_t>::max)()) == CapturePumpScheduler::c_maxFiniteWaitMilliseconds,
        "saturated deadline waits without a zero-timeout spin");
}

void TestCadencePreservesPumpHeadroom()
{
    const auto oldCadence = SimulateRelativeTenMillisecondWait();
    const auto deadlineCadence = SimulateAbsoluteActiveCadence();
    PrintResult("relative_10ms_after_work", oldCadence, false);
    PrintResult("absolute_5ms_active", deadlineCadence, true);

    RequireFrameConservation(oldCadence);
    RequireFrameConservation(deadlineCadence);
    Require(oldCadence.maximumPostPumpBacklog > deadlineCadence.maximumPostPumpBacklog,
        "absolute active cadence reduces the source backlog after whole-period pumps");
    Require(oldCadence.pendingFrames > deadlineCadence.pendingFrames, "absolute active cadence reduces pending source frames");
    Require(deadlineCadence.pumpCount > oldCadence.pumpCount, "short active cadence services the source more often");
    Require(deadlineCadence.lateTicks > 0 && deadlineCadence.skippedDeadlines > 0, "wake jitter and processing stalls are counted");
}
} // namespace

int main()
{
    TestAbsoluteDeadlinesAndLongStall();
    TestCadencePreservesPumpHeadroom();
    return 0;
}
