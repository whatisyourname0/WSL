// Copyright (C) Microsoft Corporation. All rights reserved.

#include "../CaptureHostTimingDiagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

using wsl::virtio_sound::process_host::detail::CaptureHostTimingRing;

static void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

static bool Record(
    CaptureHostTimingRing& ring,
    std::uint64_t waitStartedAtNanoseconds,
    std::uint32_t requestedWaitMilliseconds,
    std::uint64_t waitReturnedAtNanoseconds,
    std::uint64_t serviceStartedAtNanoseconds,
    std::uint64_t serviceDurationNanoseconds,
    std::uint8_t waitReason = 0,
    std::int32_t frames = 0,
    std::uint64_t generation = 0,
    std::uint32_t waitResult = 0)
{
    return ring.RecordIfSlow(
        waitStartedAtNanoseconds,
        requestedWaitMilliseconds,
        waitReturnedAtNanoseconds,
        waitResult,
        serviceStartedAtNanoseconds,
        serviceDurationNanoseconds,
        waitReason,
        frames,
        generation);
}

int main()
{
    constexpr auto infinite = (std::numeric_limits<std::uint32_t>::max)();
    CaptureHostTimingRing ring;
    Require(
        !Record(ring, 1'000, infinite, 100'001'000, 100'001'000, 0),
        "long infinite idle waits are excluded");
    Require(
        !Record(ring, 1'000, 10, 10'501'000, 10'501'000, CaptureHostTimingRing::c_slowServiceThresholdNanoseconds),
        "small wait jitter and a service exactly at the threshold are excluded");
    Require(ring.Count() == 0, "normal and expected idle waits do not use ring capacity");

    constexpr std::uint64_t waitStarted = 20'000'000;
    constexpr std::uint64_t waitReturned = waitStarted + 11'500'000;
    Require(
        Record(ring, waitStarted, 10, waitReturned, waitReturned, 1, 6, 480, 9, 2),
        "a finite wait that misses its deadline is recorded");
    const auto* deadline = ring.Oldest(0);
    Require(deadline != nullptr, "deadline sample is retained");
    Require(deadline->m_sequence == 0, "the first slow sample has sequence zero");
    Require(deadline->m_waitStartedAtNanoseconds == waitStarted, "wait start is relative to the monotonic origin");
    Require(deadline->m_requestedWaitNanoseconds == 10'000'000, "requested wait duration is retained");
    Require(deadline->m_waitReturnedAtNanoseconds == waitReturned, "actual wait return timestamp is retained");
    Require(deadline->m_actualWaitNanoseconds == 11'500'000, "actual wait duration is retained");
    Require(deadline->m_flags == CaptureHostTimingRing::c_deadlineOverrun, "deadline event reason is retained");
    Require(deadline->m_waitReason == 6 && deadline->m_frames == 480 && deadline->m_generation == 9, "service metadata is retained");
    Require(deadline->m_waitResult == 2, "wait result is retained even when an event arrives after the deadline");

    Require(
        Record(
            ring,
            30'000'000,
            infinite,
            30'100'000,
            30'100'000,
            CaptureHostTimingRing::c_slowServiceThresholdNanoseconds + 1,
            5,
            960,
            10),
        "service duration above 20ms is recorded during an otherwise infinite wait");
    const auto* service = ring.Oldest(1);
    Require(service != nullptr && service->m_sequence == 1, "events remain in insertion order");
    Require(service->m_flags == CaptureHostTimingRing::c_slowService, "slow service reason is retained");
    Require(service->m_requestedWaitNanoseconds == (std::numeric_limits<std::uint64_t>::max)(), "infinite wait is identified in the sample");

    CaptureHostTimingRing wrapped;
    for (std::size_t i = 0; i < CaptureHostTimingRing::c_capacity + 5; ++i)
    {
        const auto tick = static_cast<std::uint64_t>(i) * 50'000'000;
        Require(
            Record(
                wrapped,
                tick,
                infinite,
                tick + 1'000'000,
                tick + 1'000'000,
                CaptureHostTimingRing::c_slowServiceThresholdNanoseconds + 1,
                static_cast<std::uint8_t>(i % 7),
                static_cast<std::int32_t>(i),
                100 + i),
            "slow samples are recorded while the ring wraps");
    }
    Require(wrapped.Count() == CaptureHostTimingRing::c_capacity, "ring capacity is fixed at 32 samples");
    Require(wrapped.TotalRecorded() == CaptureHostTimingRing::c_capacity + 5, "total count includes overwritten samples");
    Require(wrapped.Oldest(CaptureHostTimingRing::c_capacity) == nullptr, "ring exposes only the bounded retained range");
    for (std::size_t i = 0; i < CaptureHostTimingRing::c_capacity; ++i)
    {
        const auto* event = wrapped.Oldest(i);
        const auto expected = static_cast<std::uint64_t>(i + 5);
        Require(event != nullptr && event->m_sequence == expected, "wrapped ring returns samples oldest to newest");
        Require(event->m_frames == static_cast<std::int32_t>(expected), "wrapped samples retain their associated frames");
        Require(event->m_generation == 100 + expected, "wrapped samples retain their generation");
    }

    std::cout << "PASS: slow-event thresholds, idle exclusion, metadata and bounded ring ordering\n";
}
