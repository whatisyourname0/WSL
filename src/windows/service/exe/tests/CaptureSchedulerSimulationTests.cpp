// Copyright (C) Microsoft Corporation. All rights reserved.
// Deterministic virtual-clock workload; no Windows device, audio, or wall sleep.
#ifndef CAPTURE_SCHEDULER_HEADER
#define CAPTURE_SCHEDULER_HEADER "../CaptureEventScheduler.hpp"
#endif
#include CAPTURE_SCHEDULER_HEADER
#include <algorithm>
#include <cstdint>
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

static void Simulate(std::uint32_t period, bool stalls)
{
    constexpr std::uint64_t millisecond = 1'000'000;
    constexpr std::uint64_t end = 3'600'000;
    constexpr auto infinite = (std::numeric_limits<std::uint64_t>::max)();
    CaptureEventScheduler scheduler;
    std::uint64_t produced{}, delivered{}, queued{}, maximumQueued{}, services{}, empty{}, sourceWakes{}, deadlineWakes{};
    std::uint64_t deadline = infinite, previousService{};
    bool dataPending = false;
    bool deferData = false;
    // 10 ms source packets, independently of the guest's negotiated period.
    // An RX-starved interval and a 200 ms host stall occur once per run.
    for (std::uint64_t ms = 0; ms <= end; ++ms)
    {
        const auto now = ms * millisecond;
        if (ms != 0 && ms % 10 == 0)
        {
            produced += 480;
            queued += 480;
            maximumQueued = (std::max)(maximumQueued, queued);
            dataPending = true;
        }
        if (stalls && ms >= 10'000 && ms < 10'200)
        {
            continue;
        }
        const bool hasRx = !stalls || ms < 20'000 || ms >= 20'150;
        const bool guestQueueEvent = stalls && ms == 20'150;
        // The host chooses a fixed wait set before blocking. A deadline does
        // not add a previously excluded handle to that in-flight wait.
        const bool dataWake = dataPending && !deferData;
        if (!dataWake && now < deadline && !guestQueueEvent)
        {
            continue;
        }
        sourceWakes += dataWake;
        deadlineWakes += !dataWake && now >= deadline;
        dataPending = false; // Source state is rescanned after consuming the event.
        ++services;
        const bool allowed = scheduler.CanTransfer(now, 1);
        CaptureServiceResult result;
        result.m_generation = 1;
        result.m_sampleRate = 48000;
        result.m_periodFrames = period;
        result.m_sourceClocked = true;
        if (allowed && hasRx && queued >= period)
        {
            result.m_frames = static_cast<int>(period);
            queued -= period;
            delivered += period;
        }
        result.m_sourceAvailableFrames = static_cast<std::uint32_t>(queued);
        result.m_moreSourceFrames = queued >= period;
        result.m_waitReason = result.m_frames > 0 ? CaptureWaitReason::Progress
                              : !allowed          ? CaptureWaitReason::PacingDeadline
                              : queued < period   ? CaptureWaitReason::SourceData
                                                  : CaptureWaitReason::RxBuffer;
        empty += result.m_frames == 0;
        scheduler.OnService(now, result);
        const auto timeout = scheduler.WaitTimeoutMilliseconds(now);
        deadline = timeout == (std::numeric_limits<std::uint32_t>::max)() ? infinite : now + timeout * millisecond;
#ifndef CAPTURE_SCHEDULER_BASELINE
        deferData = scheduler.DeferSourceData(now);
#endif
        Require(result.m_frames <= static_cast<int>(period), "No service may burst more than one negotiated period");
        Require(produced == delivered + queued, "No PCM may be dropped, duplicated, or invented by pacing");
        Require(queued < 48000, "Bounded stalls must not exhaust the one-second simulated source queue");
        Require(previousService != now || now == 0, "No repeated service at one clock tick");
        previousService = now;
    }
    Require(delivered > 172'700'000, "Hour simulation must sustain the capture rate after bounded stalls");
#ifndef CAPTURE_SCHEDULER_BASELINE
    // A deadline may precede the final 10 ms source packet. Allow one empty
    // rescan per completion, then require an event rather than timer polling.
    Require(services < 2 * (delivered / period) + 100, "Hardware capture must not rescan on every partial packet");
#endif
    std::cout << "{\"period_frames\":" << period << ",\"virtual_seconds\":3600,\"stalls\":" << (stalls ? "true" : "false")
              << ",\"produced_frames\":" << produced << ",\"delivered_frames\":" << delivered << ",\"remaining_frames\":" << queued
              << ",\"maximum_source_frames\":" << maximumQueued << ",\"services\":" << services << ",\"empty_services\":" << empty
              << ",\"source_wakes\":" << sourceWakes << ",\"deadline_wakes\":" << deadlineWakes << "}\n";
}

int main()
{
    for (const auto period : {480U, 2048U})
    {
        Simulate(period, false);
        Simulate(period, true);
    }
}
