// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "CaptureWakeSignal.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using wsl::virtio_sound::native::windows::CaptureWakeReason;
using wsl::virtio_sound::native::windows::CaptureWakeSignal;

constexpr std::uint32_t c_sourceData = static_cast<std::uint32_t>(CaptureWakeReason::SourceData);
constexpr std::uint32_t c_sourceState = static_cast<std::uint32_t>(CaptureWakeReason::SourceState);
constexpr std::uint32_t c_guestQueue = static_cast<std::uint32_t>(CaptureWakeReason::GuestQueue);

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class PumpWaitSet final
{
public:
    PumpWaitSet() :
        m_stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        m_parent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        m_prepare(CreateEventW(nullptr, FALSE, FALSE, nullptr))
    {
        Require(
            m_stop.get() != nullptr && m_parent.get() != nullptr && m_prepare.get() != nullptr,
            "unable to create synthetic pump events");
    }

    DWORD Wait(const CaptureWakeSignal& wake, bool deferSourceData, DWORD timeoutMilliseconds) const noexcept
    {
        const HANDLE handles[]{m_stop.get(), m_parent.get(), m_prepare.get(), wake.Event(), wake.DataEvent()};
        const DWORD count = deferSourceData ? 4U : 5U;
        return WaitForMultipleObjects(count, handles, FALSE, timeoutMilliseconds);
    }

    bool SignalStop() noexcept
    {
        return SetEvent(m_stop.get()) != FALSE;
    }
    bool SignalParentExit() noexcept
    {
        return SetEvent(m_parent.get()) != FALSE;
    }
    bool SignalPrepare() noexcept
    {
        return SetEvent(m_prepare.get()) != FALSE;
    }

private:
    wil::unique_handle m_stop;
    wil::unique_handle m_parent;
    wil::unique_handle m_prepare;
};

void ConsumeDataBeforeReasons(CaptureWakeSignal& wake)
{
    const DWORD result = WaitForSingleObject(wake.DataEvent(), 0);
    Require(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, "unable to drain synthetic data event");
}

void TestDataWakeIsRetainedWhileDeferred()
{
    CaptureWakeSignal wake;
    PumpWaitSet waits;
    wake.Notify(CaptureWakeReason::SourceData);

    Require(waits.Wait(wake, true, 0) == WAIT_TIMEOUT, "data-only notification woke a deferred wait");
    Require(waits.Wait(wake, false, 0) == WAIT_OBJECT_0 + 4, "deferred data notification was not retained until rescan");
    ConsumeDataBeforeReasons(wake);
    Require((wake.Consume() & c_sourceData) != 0, "deferred data reason was lost before rescan");
}

void TestDeadlineDrainsDataBeforeReasonExchange()
{
    CaptureWakeSignal wake;
    PumpWaitSet waits;
    wake.Notify(CaptureWakeReason::SourceData);

    Require(waits.Wait(wake, true, 0) == WAIT_TIMEOUT, "deferred source data woke before its deadline");
    ConsumeDataBeforeReasons(wake);
    Require((wake.Consume() & c_sourceData) != 0, "deadline rescan lost the pending source data reason");
    Require(waits.Wait(wake, false, 0) == WAIT_TIMEOUT, "deadline rescan left a stale data event queued");
}

void TestUrgentStatePreemptsDeferredData()
{
    CaptureWakeSignal wake;
    PumpWaitSet waits;
    wake.Notify(CaptureWakeReason::SourceData);
    Require(waits.Wait(wake, true, 0) == WAIT_TIMEOUT, "source data caused an OS wake before its rescan deadline");
    wake.Notify(CaptureWakeReason::SourceState);

    Require(waits.Wait(wake, true, 0) == WAIT_OBJECT_0 + 3, "source state did not preempt deferred source data");
    const auto reasons = wake.Consume();
    Require((reasons & c_sourceState) != 0, "urgent source state reason was lost");
    Require((reasons & c_sourceData) != 0, "queued source data was not included in the urgent rescan");
    Require(
        waits.Wait(wake, false, 0) == WAIT_OBJECT_0 + 4,
        "early urgent service consumed the deferred data event without a new packet");
    ConsumeDataBeforeReasons(wake);
    Require(wake.Consume() == 0, "already-consumed source data reason reappeared after its retained event");
}

void TestSourceStateUsesTheUrgentEvent()
{
    CaptureWakeSignal wake;
    wake.Notify(CaptureWakeReason::SourceState);

    Require(WaitForSingleObject(wake.Event(), 0) == WAIT_OBJECT_0, "source state did not signal the urgent event");
    Require(WaitForSingleObject(wake.DataEvent(), 0) == WAIT_TIMEOUT, "source state incorrectly signaled the data event");
    Require((wake.Consume() & c_sourceState) != 0, "source state reason was lost");
}

void TestGuestQueueResumesRetainedData()
{
    CaptureWakeSignal wake;
    PumpWaitSet waits;
    wake.Notify(CaptureWakeReason::SourceData);

    Require(waits.Wait(wake, true, 0) == WAIT_TIMEOUT, "source data caused a wake while no RX service was possible");
    wake.Notify(CaptureWakeReason::GuestQueue);
    Require(waits.Wait(wake, true, 0) == WAIT_OBJECT_0 + 3, "guest RX queue kick did not wake while data was deferred");
    const auto reasons = wake.Consume();
    Require((reasons & c_guestQueue) != 0, "guest RX queue reason was lost");
    Require((reasons & c_sourceData) != 0, "retained source data was not included in the guest RX rescan");
    Require(waits.Wait(wake, false, 0) == WAIT_OBJECT_0 + 4, "guest RX rescan lost the retained data event without a new packet");
    ConsumeDataBeforeReasons(wake);
    Require(wake.Consume() == 0, "source data reason reappeared after the guest RX rescan");
}

void TestStopParentAndPrepareRemainWaitable()
{
    {
        CaptureWakeSignal wake;
        PumpWaitSet waits;
        wake.Notify(CaptureWakeReason::SourceData);
        Require(waits.SignalStop(), "unable to signal synthetic stop event");
        Require(waits.Wait(wake, true, 0) == WAIT_OBJECT_0, "deferred data hid the stop event");
    }
    {
        CaptureWakeSignal wake;
        PumpWaitSet waits;
        wake.Notify(CaptureWakeReason::SourceData);
        Require(waits.SignalParentExit(), "unable to signal synthetic parent event");
        Require(waits.Wait(wake, true, 0) == WAIT_OBJECT_0 + 1, "deferred data hid parent exit");
    }
    {
        CaptureWakeSignal wake;
        PumpWaitSet waits;
        wake.Notify(CaptureWakeReason::SourceData);
        Require(waits.SignalPrepare(), "unable to signal synthetic prepare event");
        Require(waits.Wait(wake, true, 0) == WAIT_OBJECT_0 + 2, "deferred data hid the guest-stop prepare event");
    }
}

void TestConcurrentProducerAndRescanDoNotLoseWork()
{
    constexpr std::uint32_t c_workItems = 1000;
    constexpr DWORD c_wakeTimeoutMilliseconds = 2000;
    CaptureWakeSignal wake;
    PumpWaitSet waits;
    std::atomic<std::uint32_t> publishedSequence{};
    std::atomic<std::uint32_t> acknowledgedSequence{};
    std::atomic<bool> cancelled{};
    std::atomic<bool> producerFinished{};

    std::thread producer([&] {
        for (std::uint32_t sequence = 1; sequence <= c_workItems && !cancelled.load(std::memory_order_acquire); ++sequence)
        {
            publishedSequence.store(sequence, std::memory_order_release);
            wake.Notify(CaptureWakeReason::SourceData);
            while (acknowledgedSequence.load(std::memory_order_acquire) < sequence && !cancelled.load(std::memory_order_acquire))
            {
                (void)SwitchToThread();
            }
        }
        producerFinished.store(true, std::memory_order_release);
    });

    const char* failure = nullptr;
    for (std::uint32_t expectedSequence = 1; expectedSequence <= c_workItems; ++expectedSequence)
    {
        // Data is enabled in this wait. The timeout detects a lost wake; it is
        // never used as a substitute rescan. The producer waits for this ACK
        // before publishing another sequence, including the final write.
        const DWORD wait = waits.Wait(wake, false, c_wakeTimeoutMilliseconds);
        if (wait != WAIT_OBJECT_0 + 4)
        {
            failure = "consumer did not receive a data event for the published sequence";
            break;
        }

        const DWORD drain = WaitForSingleObject(wake.DataEvent(), 0);
        if (drain != WAIT_OBJECT_0 && drain != WAIT_TIMEOUT)
        {
            failure = "consumer could not drain the data event before reason exchange";
            break;
        }
        const auto reasons = wake.Consume();
        const auto observedSequence = publishedSequence.load(std::memory_order_acquire);
        if ((reasons & c_sourceData) == 0 || observedSequence != expectedSequence)
        {
            failure = "consumer rescan did not observe the notified sequence";
            break;
        }
        acknowledgedSequence.store(expectedSequence, std::memory_order_release);
    }

    if (failure)
    {
        cancelled.store(true, std::memory_order_release);
    }
    producer.join();
    Require(failure == nullptr, failure ? failure : "concurrent source wake failed");
    Require(producerFinished.load(std::memory_order_acquire), "synthetic producer did not finish");
    Require(
        publishedSequence.load(std::memory_order_acquire) == c_workItems && acknowledgedSequence.load(std::memory_order_acquire) == c_workItems,
        "concurrent source notifications left the final sequence unrescanned");
}

} // namespace

int main()
{
    try
    {
        TestDataWakeIsRetainedWhileDeferred();
        TestDeadlineDrainsDataBeforeReasonExchange();
        TestUrgentStatePreemptsDeferredData();
        TestSourceStateUsesTheUrgentEvent();
        TestGuestQueueResumesRetainedData();
        TestStopParentAndPrepareRemainWaitable();
        TestConcurrentProducerAndRescanDoNotLoseWork();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    std::cout << "PASS: retained data wake, urgent state preemption, lifecycle waits, and concurrent rescans.\n";
    return 0;
}
