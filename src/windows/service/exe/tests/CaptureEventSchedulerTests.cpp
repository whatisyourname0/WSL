// Copyright (C) Microsoft Corporation. All rights reserved.
#include "../CaptureEventScheduler.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace wsl::virtio_sound::native;
using wsl::virtio_sound::process_host::detail::CaptureEventScheduler;

static void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main()
{
    constexpr std::uint32_t infinite = (std::numeric_limits<std::uint32_t>::max)();
    CaptureEventScheduler scheduler;
    CaptureServiceResult result;
    result.m_generation = 1;
    result.m_sampleRate = 48000;
    result.m_periodFrames = 480;
    result.m_sourceClocked = true;
    Require(scheduler.WaitTimeoutMilliseconds(0) == infinite, "idle capture has no periodic wakeup");
    Require(scheduler.CanTransfer(0, 1), "first hardware period can be serviced immediately");
    result.m_frames = 480;
    result.m_waitReason = CaptureWaitReason::Progress;
    scheduler.OnService(0, result);
    Require(scheduler.WaitTimeoutMilliseconds(0) == 10, "completion arms one deadline to coalesce early partial data");
    Require(scheduler.DeferSourceData(0), "partial data cannot transfer before the completion deadline");
    result.m_frames = 0;
    result.m_waitReason = CaptureWaitReason::PacingDeadline;
    scheduler.OnService(500'000, result);
    Require(
        scheduler.DeferSourceData(500'000) && scheduler.WaitTimeoutMilliseconds(500'000) == 10,
        "Early urgent rescan with a partial tail preserves the one completion deadline");
    for (unsigned n = 1; n < 10; ++n)
    {
        const auto now = static_cast<std::uint64_t>(n) * 1'000'000;
        Require(!scheduler.CanTransfer(now, 1), "repeated events cannot grant early frame credit");
        result.m_frames = 0;
        result.m_moreSourceFrames = true;
        result.m_waitReason = CaptureWaitReason::PacingDeadline;
        scheduler.OnService(now, result);
        Require(scheduler.WaitTimeoutMilliseconds(now) == 10 - n, "all coalesced work shares one absolute deadline");
    }
    Require(scheduler.CanTransfer(10'000'000, 1), "one period duration permits the next completion");
    result.m_waitReason = CaptureWaitReason::RxBuffer;
    scheduler.OnService(10'000'000, result);
    Require(scheduler.WaitTimeoutMilliseconds(10'000'000) == infinite, "PCM without RX waits without spinning");
    Require(scheduler.DeferSourceData(10'000'000), "source-only events cannot supply a missing RX buffer");
    result.m_waitReason = CaptureWaitReason::SourceData;
    result.m_moreSourceFrames = false;
    scheduler.OnService(10'000'000, result);
    Require(scheduler.WaitTimeoutMilliseconds(10'000'000) == infinite, "RX without PCM waits without spinning");
    Require(!scheduler.DeferSourceData(10'000'000), "source starvation re-enables data events without a timer");
    result.m_waitReason = CaptureWaitReason::Readiness;
    scheduler.OnService(10'000'000, result);
    Require(scheduler.WaitTimeoutMilliseconds(10'000'000) == infinite, "pending START is awakened by readiness or failure");
    Require(scheduler.CanTransfer(11'000'000, 2), "new generation discards old deadline state");
    Require(!scheduler.DeferSourceData(11'000'000), "new generation clears deferred RX/data state");
    result.m_generation = 2;
    result.m_sourceClocked = false;
    result.m_waitReason = CaptureWaitReason::PacingDeadline;
    scheduler.OnService(11'000'000, result);
    Require(scheduler.WaitTimeoutMilliseconds(11'000'000) == 10, "synthetic sources retain a sample-clock deadline");
    result.m_waitReason = CaptureWaitReason::Idle;
    scheduler.OnService(12'000'000, result);
    Require(scheduler.WaitTimeoutMilliseconds(12'000'000) == infinite, "STOP disarms synthetic clock");

    CaptureEventScheduler jittered;
    result.m_generation = 3;
    result.m_sourceClocked = true;
    result.m_moreSourceFrames = true;
    result.m_frames = 480;
    result.m_waitReason = CaptureWaitReason::Progress;
    Require(jittered.CanTransfer(0, 3), "fresh jitter scenario starts immediately");
    jittered.OnService(0, result);
    for (std::uint64_t period = 1; period <= 1000; ++period)
    {
        const auto due = period * 10'000'000;
        Require(jittered.CanTransfer(due + 500'000, 3), "sub-period wake lateness must not accumulate into throughput loss");
        jittered.OnService(due + 500'000, result);
        Require(!jittered.CanTransfer(due + 9'999'999, 3), "jitter cannot permit transfer before the next sample-clock deadline");
        Require(jittered.CanTransfer(due + 10'000'000, 3), "sample-clock phase survives repeated late wakes");
    }
    constexpr std::uint64_t resumed = 20'000'000'000;
    Require(jittered.CanTransfer(resumed, 3), "one service is allowed after a long stall");
    jittered.OnService(resumed, result);
    Require(!jittered.CanTransfer(resumed, 3), "long stall must not grant repeated immediate catch-up services");
    Require(jittered.WaitTimeoutMilliseconds(resumed) == 10, "long stall rebases one future deadline");

    result.m_generation = 4;
    result.m_frames = 240;
    Require(jittered.CanTransfer(resumed, 4), "new generation clears the long-stall deadline");
    jittered.OnService(resumed, result);
    Require(jittered.WaitTimeoutMilliseconds(resumed) == 5, "partial completion guard uses actual frames");
    constexpr auto maximumTime = (std::numeric_limits<std::uint64_t>::max)();
    Require(jittered.CanTransfer(maximumTime - 1, 4), "late transfer remains permitted near clock limit");
    jittered.OnService(maximumTime - 1, result);
    Require(jittered.WaitTimeoutMilliseconds(maximumTime) == infinite - 1, "saturated clock cannot cause zero-timeout spinning");

    CaptureEventScheduler partialStart;
    result.m_generation = 5;
    result.m_frames = 0;
    result.m_periodFrames = 2048;
    result.m_waitReason = CaptureWaitReason::SourceData;
    result.m_moreSourceFrames = false;
    partialStart.OnService(10'000'000, result);
    result.m_frames = 2048;
    result.m_waitReason = CaptureWaitReason::Progress;
    partialStart.OnService(50'000'000, result);
    Require(!partialStart.CanTransfer(60'000'000, 5), "first partial packet must not anchor the completion phase");
    Require(
        partialStart.WaitTimeoutMilliseconds(50'000'000) == 43, "first actual completion owns one full negotiated-period guard");
    std::cout << "PASS: idle/event waits, duplicate notifications, completion rate guard and synthetic deadline\n";
}
