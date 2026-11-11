// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "AudioCaptureWorker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace wsl::virtio_sound::audio_worker;

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct FakeCaptureControl
{
    struct PacketReadObservation
    {
        std::uintptr_t m_bufferAddress{};
        std::size_t m_capacity{};
        std::uint32_t m_frames{};
        std::size_t m_size{};
        bool m_discontinuity{};
        std::uint32_t m_rawFlags{};
    };

    std::atomic<int> createCalls{};
    std::atomic<int> openCalls{};
    std::atomic<int> startCalls{};
    std::atomic<int> closeCalls{};
    std::atomic<int> readCalls{};
    std::atomic<bool> openSucceeds{true};
    std::atomic<bool> startSucceeds{true};
    std::atomic<bool> closeSucceeds{true};
    PcmFormat negotiatedFormat{RequiredCaptureFormat()};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<CapturePacket> packets;
    std::vector<PacketReadObservation> readObservations;
    std::size_t successfulPacketReads{};
    std::size_t timeoutCalls{};
    bool pollutePacketOnTimeout{};
    bool requestReadFailure{};
    bool waitForStopEvent{};
    bool waitingForStopEvent{};
    std::chrono::milliseconds openDelay{};
    std::wstring openedEndpoint;
};

class FakeCaptureDevice final : public ICaptureDevice
{
public:
    explicit FakeCaptureDevice(std::shared_ptr<FakeCaptureControl> control) : control_(std::move(control))
    {
    }

    bool OpenEndpoint(const std::wstring& endpointId, PcmFormat& negotiatedFormat, CaptureError& error) override
    {
        ++control_->openCalls;
        {
            std::lock_guard<std::mutex> lock(control_->mutex);
            control_->openedEndpoint = endpointId;
        }
        if (control_->openDelay > std::chrono::milliseconds::zero())
        {
            std::this_thread::sleep_for(control_->openDelay);
        }
        if (!control_->openSucceeds.load())
        {
            error = {CaptureErrorCode::EndpointOpenFailed, 123, L"fake endpoint open failure"};
            return false;
        }
        negotiatedFormat = control_->negotiatedFormat;
        return true;
    }

    bool Start(CaptureError& error) override
    {
        ++control_->startCalls;
        if (!control_->startSucceeds.load())
        {
            error = {CaptureErrorCode::StartFailed, 456, L"fake endpoint start failure"};
            return false;
        }
        return true;
    }

    CaptureReadResult ReadPacket(std::chrono::milliseconds timeout, const std::atomic<bool>& stopRequested, CapturePacket& packet, CaptureError& error) override
    {
        ++control_->readCalls;
        std::unique_lock<std::mutex> lock(control_->mutex);
        control_->readObservations.push_back({
            reinterpret_cast<std::uintptr_t>(packet.m_s16LeBytes.data()),
            packet.m_s16LeBytes.capacity(),
            packet.m_frames,
            packet.m_s16LeBytes.size(),
            packet.m_discontinuity,
            packet.m_rawFlags});
        control_->condition.wait_for(
            lock, timeout, [&] { return !control_->packets.empty() || control_->requestReadFailure || stopRequested.load(); });
        if (stopRequested.load())
        {
            return CaptureReadResult::Stopped;
        }
        if (control_->requestReadFailure)
        {
            control_->requestReadFailure = false;
            error = {CaptureErrorCode::ReadFailed, 789, L"fake endpoint read failure"};
            return CaptureReadResult::Failed;
        }
        if (control_->packets.empty())
        {
            ++control_->timeoutCalls;
            if (control_->pollutePacketOnTimeout && control_->successfulPacketReads != 0)
            {
                packet.m_frames = 1;
                packet.m_s16LeBytes.assign(RequiredBytesPerFrame(), std::uint8_t{0xE7});
                packet.m_discontinuity = true;
                packet.m_rawFlags = 0xFFFFFFFFU;
            }
            control_->condition.notify_all();
            return CaptureReadResult::Timeout;
        }
        CapturePacket queuedPacket = std::move(control_->packets.front());
        control_->packets.pop_front();
        packet.m_frames = queuedPacket.m_frames;
        packet.m_s16LeBytes.assign(queuedPacket.m_s16LeBytes.begin(), queuedPacket.m_s16LeBytes.end());
        packet.m_discontinuity = queuedPacket.m_discontinuity;
        packet.m_rawFlags = queuedPacket.m_rawFlags;
        ++control_->successfulPacketReads;
        return CaptureReadResult::Packet;
    }

    CaptureReadResult ReadPacketInterruptible(
        std::chrono::milliseconds timeout,
        const std::atomic<bool>& stopRequested,
        std::uintptr_t stopWaitHandle,
        CapturePacket& packet,
        CaptureError& error) override
    {
        bool waitForStopEvent = false;
        {
            std::lock_guard<std::mutex> lock(control_->mutex);
            waitForStopEvent = control_->waitForStopEvent;
            if (waitForStopEvent)
            {
                control_->waitingForStopEvent = true;
            }
        }
        if (!waitForStopEvent)
        {
            // Keep the existing fake-device path and its short bounded
            // ReadPacket timeout for the rest of the lifecycle tests.
            return ICaptureDevice::ReadPacketInterruptible(timeout, stopRequested, stopWaitHandle, packet, error);
        }
        control_->condition.notify_all();
        if (stopWaitHandle == 0U)
        {
            error = {CaptureErrorCode::ReadFailed, 0, L"the fake stop event was missing"};
            return CaptureReadResult::Failed;
        }
        const DWORD result = WaitForSingleObject(reinterpret_cast<HANDLE>(stopWaitHandle), INFINITE);
        if (result == WAIT_OBJECT_0 || stopRequested.load())
        {
            return CaptureReadResult::Stopped;
        }
        error = {CaptureErrorCode::ReadFailed, static_cast<long>(HRESULT_FROM_WIN32(GetLastError())), L"fake stop-event wait failed"};
        return CaptureReadResult::Failed;
    }

    bool Close(CaptureError& error) override
    {
        ++control_->closeCalls;
        if (!control_->closeSucceeds.load())
        {
            error = {CaptureErrorCode::StopFailed, 321, L"fake endpoint close failure"};
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<FakeCaptureControl> control_;
};

class FakeCaptureDeviceFactory final : public ICaptureDeviceFactory
{
public:
    explicit FakeCaptureDeviceFactory(std::shared_ptr<FakeCaptureControl> control) : control_(std::move(control))
    {
    }

    std::unique_ptr<ICaptureDevice> Create() override
    {
        ++control_->createCalls;
        return std::make_unique<FakeCaptureDevice>(control_);
    }

private:
    std::shared_ptr<FakeCaptureControl> control_;
};

CapturePacket MakePacket(std::uint32_t frames, std::uint8_t firstByte)
{
    CapturePacket packet;
    packet.m_frames = frames;
    packet.m_s16LeBytes.resize(static_cast<std::size_t>(frames) * RequiredBytesPerFrame());
    for (std::size_t index = 0; index < packet.m_s16LeBytes.size(); ++index)
    {
        packet.m_s16LeBytes[index] = static_cast<std::uint8_t>(firstByte + index);
    }
    return packet;
}

void PushPacket(const std::shared_ptr<FakeCaptureControl>& control, CapturePacket packet)
{
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        control->packets.push_back(std::move(packet));
    }
    control->condition.notify_all();
}

void RequestReadFailure(const std::shared_ptr<FakeCaptureControl>& control)
{
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        control->requestReadFailure = true;
    }
    control->condition.notify_all();
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

std::unique_ptr<ICaptureDeviceFactory> MakeFactory(const std::shared_ptr<FakeCaptureControl>& control)
{
    return std::make_unique<FakeCaptureDeviceFactory>(control);
}

void TestResetCapturePacketRetainsBoundedStorage()
{
    CapturePacket packet;
    packet.m_s16LeBytes.reserve(MaximumCapturePacketBytes());
    packet.m_frames = 3;
    packet.m_s16LeBytes.assign(3 * RequiredBytesPerFrame(), std::uint8_t{0xA5});
    packet.m_discontinuity = true;
    packet.m_rawFlags = 0x12345678U;
    const auto capacity = packet.m_s16LeBytes.capacity();
    const auto bufferAddress = reinterpret_cast<std::uintptr_t>(packet.m_s16LeBytes.data());

    ResetCapturePacket(packet);

    Require(packet.m_frames == 0 && packet.m_s16LeBytes.empty(), "packet reset should clear frame and sample counts");
    Require(!packet.m_discontinuity && packet.m_rawFlags == 0, "packet reset should clear all metadata");
    Require(packet.m_s16LeBytes.capacity() == capacity, "packet reset should retain its bounded storage");
    Require(
        reinterpret_cast<std::uintptr_t>(packet.m_s16LeBytes.data()) == bufferAddress,
        "packet reset should keep the allocated sample buffer for reuse");
}

void TestWorkerReusesPacketStorageAndClearsTimeoutOutput()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->pollutePacketOnTimeout = true;
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "fake capture should start");

    PushPacket(control, MakePacket(2, 10));
    Require(WaitUntil([&] { return worker.Snapshot().m_framesWritten == 2; }), "first packet should be written");
    Require(
        WaitUntil([&] {
            std::lock_guard<std::mutex> lock(control->mutex);
            return control->timeoutCalls != 0;
        }),
        "the fake device should return a timeout after the first packet");

    PushPacket(control, MakePacket(2, 70));
    Require(WaitUntil([&] { return worker.Snapshot().m_framesWritten == 4; }), "second packet should be written after timeout");

    std::vector<FakeCaptureControl::PacketReadObservation> observations;
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        observations = control->readObservations;
    }
    Require(observations.size() >= 3, "the fake should observe reads before and after the timeout");
    const auto initialBufferAddress = observations.front().m_bufferAddress;
    for (const auto& observation : observations)
    {
        Require(
            observation.m_capacity >= MaximumCapturePacketBytes(),
            "capture packet storage should be reserved before the first device read");
        Require(
            observation.m_bufferAddress == initialBufferAddress,
            "successive capture reads should reuse the same sample allocation");
        Require(
            observation.m_frames == 0 && observation.m_size == 0 && !observation.m_discontinuity && observation.m_rawFlags == 0,
            "each read should begin with empty packet metadata, including after a dirty timeout");
    }

    std::vector<std::uint8_t> output(4 * RequiredBytesPerFrame());
    Require(worker.ReadFrames(output.data(), 4, output.size()) == 4, "both real packets should remain available to the consumer");
    Require(
        output[0] == 10 && output[7] == 17 && output[8] == 70 && output[15] == 77,
        "timeout output must not replace or contaminate the real captured samples");
    Require(worker.Stop(error), "fake capture should stop cleanly");
}

void TestConstructionIsInertAndStartUsesExplicitEndpoint()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 16);
    Require(control->createCalls.load() == 0, "construction must not create a capture device");
    Require(control->openCalls.load() == 0, "construction must not open an endpoint");
    Require(worker.Snapshot().m_state == WorkerState::Stopped, "new worker must begin stopped");

    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "explicit Start should succeed");
    Require(error.m_code == CaptureErrorCode::None, "successful Start must clear the error");
    Require(control->createCalls.load() == 1, "Start must create one capture device");
    Require(control->openCalls.load() == 1, "Start must open one endpoint");
    Require(control->startCalls.load() == 1, "Start must start the capture device once");
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        Require(control->openedEndpoint == L"explicit-test-endpoint", "the selected endpoint ID must be passed through unchanged");
    }

    Require(
        !worker.Start(L"", error) && error.m_code == CaptureErrorCode::Busy,
        "a second Start must be rejected before inspecting its endpoint ID");
    Require(worker.Snapshot().m_state == WorkerState::Running, "a rejected second Start must not change the active worker state");
    const CapturePacket activePacket = MakePacket(2, 40);
    PushPacket(control, activePacket);
    Require(
        WaitUntil([&] { return worker.Snapshot().m_queuedFrames == 2; }),
        "the original capture must continue after a rejected second Start");
    std::vector<std::uint8_t> activeOutput(2 * RequiredBytesPerFrame());
    Require(worker.ReadFrames(activeOutput.data(), 2, activeOutput.size()) == 2, "the active worker must keep delivering frames");
    Require(activeOutput == activePacket.m_s16LeBytes, "the rejected Start must not replace or reset active capture data");

    Require(worker.Stop(error), "Stop should close a running fake endpoint cleanly");
    Require(worker.Snapshot().m_state == WorkerState::Stopped, "worker must report stopped after Stop");
    Require(control->closeCalls.load() == 1, "Stop must close the capture device");
}

void TestQueueReadinessEventTracksQueuedFrames()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 16);
    HANDLE activityEvent = static_cast<HANDLE>(worker.ActivityWaitHandle());
    Require(activityEvent != nullptr, "worker should own a capture activity event");

    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "fake capture should start");
    Require(
        WaitForSingleObject(activityEvent, 0U) == WAIT_TIMEOUT,
        "starting an idle endpoint must not leave the queue readiness event signaled");

    PushPacket(control, MakePacket(4U, 20U));
    Require(
        WaitForSingleObject(activityEvent, 3000U) == WAIT_OBJECT_0,
        "a queued capture packet should signal the activity event without polling");
    Require(worker.Snapshot().m_activitySignals == 1U, "the accepted packet should record one activity signal");

    std::vector<std::uint8_t> output(4U * RequiredBytesPerFrame());
    Require(worker.ReadFrames(output.data(), 1U, RequiredBytesPerFrame()) == 1U, "the worker should allow a partial queue read");
    Require(
        WaitForSingleObject(activityEvent, 0U) == WAIT_OBJECT_0,
        "the event should remain signaled while unread frames remain");
    Require(
        worker.ReadFrames(output.data(), 3U, 3U * RequiredBytesPerFrame()) == 3U,
        "the rest of the queued packet should be readable");
    Require(
        WaitForSingleObject(activityEvent, 0U) == WAIT_TIMEOUT,
        "draining the queue should reset the readiness event under the queue lock");
    Require(worker.Stop(error), "Stop should close a running fake endpoint cleanly");
}

void TestStopEventInterruptsDeviceReadOnCaptureThread()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->waitForStopEvent = true;
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "interruptible fake capture should start");
    Require(
        WaitUntil([&] {
            std::lock_guard<std::mutex> lock(control->mutex);
            return control->waitingForStopEvent;
        }),
        "capture thread should enter the stop-event wait before Stop");

    Require(worker.Stop(error), "Stop should signal the wait and join the capture thread");
    Require(control->closeCalls.load() == 1, "the capture thread should close the fake device after the stop wait");
}

void TestEndpointIdIsRequired()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(!worker.Start(L"  \t", error), "blank endpoint ID must fail closed");
    Require(error.m_code == CaptureErrorCode::InvalidEndpointId, "blank endpoint error should be precise");
    Require(control->createCalls.load() == 0, "invalid endpoint must not create a device");
    Require(control->openCalls.load() == 0, "invalid endpoint must not reach endpoint activation");
}

void TestUnsupportedFormatFailsClosed()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->negotiatedFormat = {44100, 2, 16};
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(!worker.Start(L"explicit-test-endpoint", error), "unsupported negotiated format must fail Start");
    Require(error.m_code == CaptureErrorCode::UnsupportedFormat, "format mismatch should be reported as unsupported");
    Require(control->startCalls.load() == 0, "the device must not start after negotiating the wrong format");
    Require(control->closeCalls.load() == 1, "partially opened devices must be closed after format failure");
}

void TestEndpointOpenErrorIsPreserved()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->openSucceeds.store(false);
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(!worker.Start(L"explicit-test-endpoint", error), "endpoint activation error must fail Start");
    Require(error.m_code == CaptureErrorCode::EndpointOpenFailed, "endpoint activation failure must be surfaced");
    Require(error.m_nativeCode == 123, "native endpoint error code must be preserved");
    Require(control->closeCalls.load() == 1, "device cleanup must run after endpoint activation failure");
}

void TestStartupTimeoutCancelsBeforeDeviceStart()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->openDelay = 60ms;
    AudioCaptureWorker worker(MakeFactory(control), 16, 10ms);
    CaptureError error;
    Require(!worker.Start(L"explicit-test-endpoint", error), "startup should fail after its configured wait deadline");
    Require(
        error.m_code == CaptureErrorCode::StartupTimeout, "startup timeout should be distinct from endpoint activation errors");
    Require(control->openCalls.load() == 1, "timeout test should enter fake endpoint open");
    Require(control->startCalls.load() == 0, "a device returned after cancellation must not start capture");
    Require(control->closeCalls.load() == 1, "timed-out startup must close its partially opened device");
}

void TestDeviceStartFailureIsSurfaced()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->startSucceeds.store(false);
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(!worker.Start(L"explicit-test-endpoint", error), "device Start failure must fail worker startup");
    Require(
        error.m_code == CaptureErrorCode::StartFailed && error.m_nativeCode == 456,
        "device Start error code must reach the caller");
    Require(control->closeCalls.load() == 1, "device must close after its Start method fails");
}

void TestStopFailureIsSurfaced()
{
    auto control = std::make_shared<FakeCaptureControl>();
    control->closeSucceeds.store(false);
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "fake capture should start");
    Require(!worker.Stop(error), "device close failure must make Stop fail");
    Require(
        error.m_code == CaptureErrorCode::StopFailed && error.m_nativeCode == 321,
        "device close error code must reach the caller");
    Require(
        worker.Snapshot().m_lastError.m_code == CaptureErrorCode::StopFailed,
        "device close error must remain available in the worker snapshot");
}

void TestBoundedQueueOverflowFailsWithoutExposingGappedAudio()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 5);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "fake capture should start");
    PushPacket(control, MakePacket(3, 10));
    PushPacket(control, MakePacket(3, 100));
    Require(
        WaitUntil([&] { return worker.Snapshot().m_state == WorkerState::Failed; }),
        "queue overflow should fail capture instead of hiding an audio gap");

    const WorkerSnapshot snapshot = worker.Snapshot();
    Require(snapshot.m_queuedFrames == 0, "queued PCM must be discarded when overflow breaks timeline continuity");
    Require(
        snapshot.m_lastError.m_code == CaptureErrorCode::BufferOverflow,
        "overflow must be reported as a distinct capture failure");
    Require(snapshot.m_overflowEvents == 1, "overflow event must be counted");
    Require(snapshot.m_framesWritten == 3, "only accepted frames count as written");
    Require(snapshot.m_overflowFramesDropped == 3, "all frames in the rejected packet must be counted as dropped");

    std::vector<std::uint8_t> output(5 * RequiredBytesPerFrame(), 0xA5);
    const std::size_t framesRead = worker.ReadFrames(output.data(), 5, output.size());
    Require(framesRead == 0, "overflowed capture must expose no partial timeline");
    Require(
        std::all_of(output.begin(), output.end(), [](std::uint8_t value) { return value == 0xA5; }),
        "read from the cleared queue must leave consumer memory untouched");
    Require(!worker.Stop(error) && error.m_code == CaptureErrorCode::BufferOverflow, "Stop should preserve the overflow failure");
}

void TestEndpointDiscontinuityFailsAndClearsQueue()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "fake capture should start");
    CapturePacket discontinuousPacket = MakePacket(2, 90);
    discontinuousPacket.m_discontinuity = true;
    PushPacket(control, std::move(discontinuousPacket));
    Require(
        WaitUntil([&] { return worker.Snapshot().m_state == WorkerState::Failed; }),
        "capture discontinuity should fail the worker");
    const WorkerSnapshot snapshot = worker.Snapshot();
    Require(
        snapshot.m_lastError.m_code == CaptureErrorCode::CaptureDiscontinuity,
        "capture discontinuity must be visible as a distinct error");
    Require(
        snapshot.m_discontinuities == 1 && snapshot.m_queuedFrames == 0,
        "discontinuity must be counted and buffered PCM must be cleared");
    Require(
        !worker.Stop(error) && error.m_code == CaptureErrorCode::CaptureDiscontinuity,
        "Stop should preserve the discontinuity failure");
}

void TestMalformedPacketFailsAndNeverProducesFallbackAudio()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "fake capture should start");
    CapturePacket malformed;
    malformed.m_frames = 2;
    malformed.m_s16LeBytes = {1, 2, 3, 4};
    PushPacket(control, std::move(malformed));
    Require(
        WaitUntil([&] { return worker.Snapshot().m_state == WorkerState::Failed; }),
        "malformed packet should fail the capture worker");
    const WorkerSnapshot snapshot = worker.Snapshot();
    Require(snapshot.m_lastError.m_code == CaptureErrorCode::InvalidPacket, "malformed packet error must be visible");
    std::vector<std::uint8_t> output(4, 0xA5);
    Require(worker.ReadFrames(output.data(), 1, output.size()) == 0, "invalid data must not enter the queue");
    Require(output[0] == 0xA5, "the worker must leave consumer memory untouched when no real packet exists");
    Require(
        !worker.Stop(error) && error.m_code == CaptureErrorCode::InvalidPacket,
        "Stop should preserve the capture failure for the caller");
}

void TestEndpointReadFailureIsVisible()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "fake capture should start");
    RequestReadFailure(control);
    Require(
        WaitForSingleObject(static_cast<HANDLE>(worker.ActivityWaitHandle()), 3000U) == WAIT_OBJECT_0,
        "a capture failure should signal the activity event for the session owner");
    Require(
        WaitUntil([&] { return worker.Snapshot().m_state == WorkerState::Failed; }), "endpoint read error should stop the worker");
    const WorkerSnapshot snapshot = worker.Snapshot();
    Require(snapshot.m_lastError.m_code == CaptureErrorCode::ReadFailed, "endpoint read error must be surfaced");
    Require(snapshot.m_queuedFrames == 0, "failed endpoint must not substitute synthetic audio");
    Require(!worker.Stop(error) && error.m_nativeCode == 789, "native read failure should survive Stop");
    HANDLE activityEvent = static_cast<HANDLE>(worker.ActivityWaitHandle());
    Require(
        WaitForSingleObject(activityEvent, 0U) == WAIT_OBJECT_0,
        "stopping a failed epoch should preserve its terminal activity signal");
    Require(worker.Start(L"explicit-test-endpoint", error), "a new capture epoch should still start after failure cleanup");
    Require(
        WaitForSingleObject(activityEvent, 0U) == WAIT_TIMEOUT,
        "starting a new epoch should reset the prior terminal signal");
    Require(worker.Stop(error), "the restarted fake endpoint should stop cleanly");
}

void TestQueueResetAndWorkerCanRestart()
{
    auto control = std::make_shared<FakeCaptureControl>();
    AudioCaptureWorker worker(MakeFactory(control), 16);
    CaptureError error;
    Require(worker.Start(L"explicit-test-endpoint", error), "first Start should succeed");
    PushPacket(control, MakePacket(2, 20));
    Require(WaitUntil([&] { return worker.Snapshot().m_queuedFrames == 2; }), "first epoch packet should be queued");
    Require(worker.Stop(error), "first Stop should succeed");
    Require(worker.Snapshot().m_queuedFrames == 0, "Stop must discard buffered frames from the old capture epoch");
    Require(worker.Start(L"explicit-test-endpoint", error), "worker should restart after Stop");
    Require(control->startCalls.load() == 2, "restart should create a new capture stream");
    Require(worker.Stop(error), "second Stop should succeed");
}

} // namespace

int main()
{
    try
    {
        TestResetCapturePacketRetainsBoundedStorage();
        TestWorkerReusesPacketStorageAndClearsTimeoutOutput();
        TestConstructionIsInertAndStartUsesExplicitEndpoint();
        TestQueueReadinessEventTracksQueuedFrames();
        TestStopEventInterruptsDeviceReadOnCaptureThread();
        TestEndpointIdIsRequired();
        TestUnsupportedFormatFailsClosed();
        TestEndpointOpenErrorIsPreserved();
        TestStartupTimeoutCancelsBeforeDeviceStart();
        TestDeviceStartFailureIsSurfaced();
        TestStopFailureIsSurfaced();
        TestBoundedQueueOverflowFailsWithoutExposingGappedAudio();
        TestEndpointDiscontinuityFailsAndClearsQueue();
        TestMalformedPacketFailsAndNeverProducesFallbackAudio();
        TestEndpointReadFailureIsVisible();
        TestQueueResetAndWorkerCanRestart();
        std::cout << "AudioCaptureWorker tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AudioCaptureWorker test failure: " << error.what() << '\n';
        return 1;
    }
}
