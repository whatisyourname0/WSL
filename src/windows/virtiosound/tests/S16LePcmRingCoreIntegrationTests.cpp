// Copyright (C) Microsoft Corporation. All rights reserved.

#include "S16LePcmRing.hpp"
#include "VirtioSoundDevice.hpp"
#include "../../service/exe/CaptureEventScheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using wsl::virtio_sound::native::DiagnosticSnapshot;
using wsl::virtio_sound::native::GuestMemoryCallbacks;
using wsl::virtio_sound::native::InterruptCallbacks;
using wsl::virtio_sound::native::PcmFormat;
using wsl::virtio_sound::native::PcmRingWriteResult;
using wsl::virtio_sound::native::S16LePcmRingSource;
using wsl::virtio_sound::native::VirtioSoundDevice;

namespace {
struct FakeGuest
{
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(4 * 1024 * 1024);
};

void Assert(bool condition, const char* message, int& assertions)
{
    ++assertions;
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool ReadGuest(void* context, std::uint64_t address, std::uint8_t* destination, std::size_t count)
{
    auto& guest = *static_cast<FakeGuest*>(context);
    if (address > guest.memory.size() || count > guest.memory.size() - static_cast<std::size_t>(address))
    {
        return false;
    }
    if (count != 0)
    {
        std::copy_n(guest.memory.data() + static_cast<std::size_t>(address), count, destination);
    }
    return true;
}

bool WriteGuest(void* context, std::uint64_t address, const std::uint8_t* source, std::size_t count)
{
    auto& guest = *static_cast<FakeGuest*>(context);
    if (address > guest.memory.size() || count > guest.memory.size() - static_cast<std::size_t>(address))
    {
        return false;
    }
    if (count != 0)
    {
        std::copy_n(source, count, guest.memory.data() + static_cast<std::size_t>(address));
    }
    return true;
}

bool DeliverMsi(void*, std::uint64_t, std::uint32_t)
{
    return true;
}

void Put(FakeGuest& guest, std::uint64_t address, std::uint64_t value, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i)
    {
        guest.memory[static_cast<std::size_t>(address) + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

std::uint64_t Get(const FakeGuest& guest, std::uint64_t address, std::size_t length)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < length; ++i)
    {
        value |= static_cast<std::uint64_t>(guest.memory[static_cast<std::size_t>(address) + i]) << (8 * i);
    }
    return value;
}

void BarWrite(VirtioSoundDevice& device, std::uint64_t offset, std::uint64_t value, std::size_t width)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < width; ++i)
    {
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    device.WriteBar(offset, bytes.data(), width);
}

void ConfigureQueue(VirtioSoundDevice& device, std::uint16_t index, std::uint64_t descriptor, std::uint64_t available, std::uint64_t used)
{
    BarWrite(device, 22, index, 2);
    BarWrite(device, 26, 0xffff, 2);
    BarWrite(device, 32, descriptor, 8);
    BarWrite(device, 40, available, 8);
    BarWrite(device, 48, used, 8);
    BarWrite(device, 28, 1, 2);
}

void Negotiate(VirtioSoundDevice& device)
{
    BarWrite(device, 0, 1, 4);
    BarWrite(device, 8, 1, 4);
    BarWrite(device, 12, 1, 4);
    BarWrite(device, 20, 1, 1);
    BarWrite(device, 20, 3, 1);
    BarWrite(device, 20, 11, 1);
    BarWrite(device, 20, 15, 1);
}

void PutDescriptor(FakeGuest& guest, std::uint64_t table, std::uint16_t index, std::uint64_t address, std::uint32_t length, std::uint16_t flags, std::uint16_t next)
{
    const std::uint64_t at = table + 16ULL * index;
    Put(guest, at, address, 8);
    Put(guest, at + 8, length, 4);
    Put(guest, at + 12, flags, 2);
    Put(guest, at + 14, next, 2);
}

void PublishHead(FakeGuest& guest, std::uint64_t available, std::uint16_t slot, std::uint16_t availableIndex, std::uint16_t head)
{
    Put(guest, available + 4 + 2ULL * slot, head, 2);
    Put(guest, available + 2, availableIndex, 2);
}

void SubmitControl(FakeGuest& guest, VirtioSoundDevice& device, std::uint16_t availableIndex, const std::uint8_t* request, std::size_t requestLength, int& assertions)
{
    std::copy_n(request, requestLength, guest.memory.begin() + 0x4000);
    PutDescriptor(guest, 0x1000, 0, 0x4000, static_cast<std::uint32_t>(requestLength), 1, 1);
    PutDescriptor(guest, 0x1000, 1, 0x5000, 4, 2, 0);
    PublishHead(guest, 0x2000, static_cast<std::uint16_t>(availableIndex - 1), availableIndex, 0);
    Assert(device.ProcessControlQueue() == 1, "Virtio PCM control command completes", assertions);
    Assert(Get(guest, 0x5000, 4) == 0x8000, "Virtio PCM control command returns OK", assertions);
}

void SetUpDevice(FakeGuest& guest, VirtioSoundDevice& device, std::uint32_t bufferBytes, std::uint32_t periodBytes, int& assertions)
{
    Negotiate(device);
    ConfigureQueue(device, 0, 0x1000, 0x2000, 0x3000);
    ConfigureQueue(device, 3, 0x10000, 0x12000, 0x13000);

    std::array<std::uint8_t, 24> setParams{};
    setParams[0] = 1;
    setParams[1] = 1; // PCM_SET_PARAMS (0x0101)
    for (std::size_t i = 0; i < 4; ++i)
    {
        setParams[8 + i] = static_cast<std::uint8_t>(bufferBytes >> (8 * i));
        setParams[12 + i] = static_cast<std::uint8_t>(periodBytes >> (8 * i));
    }
    setParams[20] = 2; // stereo
    setParams[21] = 5; // S16_LE
    setParams[22] = 7; // 48 kHz
    SubmitControl(guest, device, 1, setParams.data(), setParams.size(), assertions);

    std::array<std::uint8_t, 8> command{};
    command[0] = 2;
    command[1] = 1; // PCM_PREPARE
    SubmitControl(guest, device, 2, command.data(), command.size(), assertions);
    command[0] = 4; // PCM_START
    SubmitControl(guest, device, 3, command.data(), command.size(), assertions);
}

void EncodeStereoRamp(std::uint32_t sequence, std::uint8_t* destination)
{
    const std::uint16_t left = static_cast<std::uint16_t>(sequence);
    const std::uint16_t right = static_cast<std::uint16_t>(sequence ^ 0xa55aU);
    destination[0] = static_cast<std::uint8_t>(left);
    destination[1] = static_cast<std::uint8_t>(left >> 8);
    destination[2] = static_cast<std::uint8_t>(right);
    destination[3] = static_cast<std::uint8_t>(right >> 8);
}

void FillStereoFrames(std::uint32_t first, std::uint32_t count, std::uint8_t* destination)
{
    for (std::uint32_t frame = 0; frame < count; ++frame)
    {
        EncodeStereoRamp(first + frame, destination + static_cast<std::size_t>(frame) * 4);
    }
}

void EncodeSequenceFrame(std::uint32_t sequence, std::uint8_t* destination)
{
    const std::uint16_t left = static_cast<std::uint16_t>(sequence);
    const std::uint16_t right = static_cast<std::uint16_t>(sequence >> 16);
    destination[0] = static_cast<std::uint8_t>(left);
    destination[1] = static_cast<std::uint8_t>(left >> 8);
    destination[2] = static_cast<std::uint8_t>(right);
    destination[3] = static_cast<std::uint8_t>(right >> 8);
}

void QueueCaptureDescriptors(FakeGuest& guest, std::uint16_t count, std::uint32_t payloadBytes)
{
    const std::uint32_t stride = payloadBytes + 8;
    for (std::uint16_t i = 0; i < count; ++i)
    {
        const std::uint16_t requestIndex = static_cast<std::uint16_t>(i * 2);
        const std::uint64_t requestAddress = 0x20000 + static_cast<std::uint64_t>(i) * 4;
        const std::uint64_t payloadAddress = 0x30000 + static_cast<std::uint64_t>(i) * stride;
        Put(guest, requestAddress, 0, 4); // capture stream ID
        PutDescriptor(guest, 0x10000, requestIndex, requestAddress, 4, 1, static_cast<std::uint16_t>(requestIndex + 1));
        PutDescriptor(guest, 0x10000, static_cast<std::uint16_t>(requestIndex + 1), payloadAddress, stride, 2, 0);
        PublishHead(guest, 0x12000, i, static_cast<std::uint16_t>(i + 1), requestIndex);
    }
}

void RepostCaptureDescriptor(FakeGuest& guest, std::uint16_t bufferSlot, std::uint16_t& availableIndex, std::uint32_t payloadBytes)
{
    constexpr std::uint16_t c_queueEntries = VirtioSoundDevice::c_maxQueueSize;
    const std::uint16_t requestIndex = static_cast<std::uint16_t>(bufferSlot * 2);
    const std::uint32_t stride = payloadBytes + 8;
    const std::uint64_t requestAddress = 0x20000 + static_cast<std::uint64_t>(bufferSlot) * 4;
    const std::uint64_t payloadAddress = 0x30000 + static_cast<std::uint64_t>(bufferSlot) * stride;
    Put(guest, requestAddress, 0, 4);
    PutDescriptor(guest, 0x10000, requestIndex, requestAddress, 4, 1, static_cast<std::uint16_t>(requestIndex + 1));
    PutDescriptor(guest, 0x10000, static_cast<std::uint16_t>(requestIndex + 1), payloadAddress, stride, 2, 0);
    const std::uint16_t ringSlot = static_cast<std::uint16_t>(availableIndex % c_queueEntries);
    ++availableIndex;
    PublishHead(guest, 0x12000, ringSlot, availableIndex, requestIndex);
}

std::unique_ptr<VirtioSoundDevice> CreateDevice(FakeGuest& guest, const std::shared_ptr<wsl::virtio_sound::native::PcmSource>& ring)
{
    GuestMemoryCallbacks memory{&guest, &ReadGuest, &WriteGuest};
    InterruptCallbacks interrupts{&guest, &DeliverMsi, nullptr};
    return std::make_unique<VirtioSoundDevice>(memory, interrupts, ring, PcmFormat{2, 48000});
}

// A hardware-clocked adapter around the real ring. The plain ring deliberately
// leaves AvailableFrames unknown; using it directly exercises synthetic pacing.
class ClockedRingSource final : public wsl::virtio_sound::native::PcmSource
{
public:
    explicit ClockedRingSource(std::shared_ptr<S16LePcmRingSource> ring) : m_ring(std::move(ring)) {}
    bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t bytes) noexcept override
    {
        return m_ring->ReadFrames(frames, destination, bytes);
    }
    void Reset() noexcept override { m_ring->Reset(); }
    std::optional<std::uint32_t> AvailableFrames() const noexcept override
    {
        if (m_hiddenQueries != 0)
        {
            --m_hiddenQueries;
            return m_visibleFrames;
        }
        return static_cast<std::uint32_t>(m_ring->Snapshot().m_queuedFrames);
    }
    void HideNextQueries(unsigned count, std::uint32_t visibleFrames) noexcept
    {
        m_hiddenQueries = count;
        m_visibleFrames = visibleFrames;
    }

private:
    std::shared_ptr<S16LePcmRingSource> m_ring;
    mutable unsigned m_hiddenQueries{};
    std::uint32_t m_visibleFrames{};
};

void TestSourceArrivalBetweenServiceSnapshots(std::uint32_t initialFrames, int& assertions)
{
    using wsl::virtio_sound::native::CaptureWaitReason;
    using wsl::virtio_sound::process_host::detail::CaptureEventScheduler;
    constexpr std::uint32_t period = 2048;
    FakeGuest guest;
    auto ring = std::make_shared<S16LePcmRingSource>(PcmFormat{2, 48000}, period * 4);
    auto source = std::make_shared<ClockedRingSource>(ring);
    auto device = CreateDevice(guest, source);
    SetUpDevice(guest, *device, period * 4 * 4, period * 4, assertions);
    QueueCaptureDescriptors(guest, 4, period * 4);
    std::vector<std::uint8_t> packet(period * 4);
    FillStereoFrames(0, period, packet.data());
    Assert(ring->TryWriteFrames(ring->Generation(), packet.data(), period, packet.size()) == PcmRingWriteResult::Written,
        "Race fixture publishes one complete period", assertions);
    // Observe less than one period for the histogram and pump budget, then expose the
    // producer's arrival to the post-service snapshot, without a new RX kick.
    source->HideNextQueries(2, initialFrames);
    const auto raced = device->ServiceCapture(0, true);
    Assert(raced.m_frames == 0 && raced.m_sourceAvailableFrames == period,
        "Producer arrival crosses the period threshold after the pump budget snapshot", assertions);
    CaptureEventScheduler scheduler;
    scheduler.OnService(0, raced);
    Assert(raced.m_waitReason == CaptureWaitReason::SourceData && !scheduler.DeferSourceData(0),
        "A source snapshot race must not suppress data while already-posted RX buffers wait", assertions);
    const auto resumed = device->ServiceCapture(0, scheduler.CanTransfer(0, raced.m_generation));
    Assert(resumed.m_frames == static_cast<int>(period) && Get(guest, 0x13000 + 2, 2) == 1,
        "A subsequent source wake completes the existing RX buffer without another guest kick", assertions);
    Assert(std::equal(packet.begin(), packet.end(), guest.memory.begin() + 0x30000),
        "Resumed capture preserves every PCM byte", assertions);
    Assert(ring->Snapshot().m_queuedFrames == 0 && device->GetDiagnosticsSnapshot().m_rxStatusOnlyCompletions == 0,
        "Snapshot race causes neither queued residue nor a false XRUN", assertions);
}

void TestConcurrentProducerAndCorePump(int& assertions)
{
    constexpr std::uint32_t c_periodFrames = 4096;
    constexpr std::uint16_t c_queueEntries = 64;
    constexpr std::uint32_t c_totalFrames = c_periodFrames * c_queueEntries;
    constexpr std::uint32_t c_periodBytes = c_periodFrames * 4;
    constexpr std::uint32_t c_totalBytes = c_totalFrames * 4;
    constexpr std::size_t c_ringCapacityFrames = static_cast<std::size_t>(c_totalFrames) * 2;

    FakeGuest guest;
    auto ring = std::make_shared<S16LePcmRingSource>(PcmFormat{2, 48000}, c_ringCapacityFrames);
    auto device = CreateDevice(guest, ring);
    SetUpDevice(guest, *device, static_cast<std::uint32_t>(c_ringCapacityFrames * 4), c_periodBytes, assertions);

    const std::uint64_t generation = ring->Generation();
    std::vector<std::uint8_t> initial(c_totalBytes);
    std::vector<std::uint8_t> incoming(c_totalBytes);
    FillStereoFrames(0, c_totalFrames, initial.data());
    FillStereoFrames(c_totalFrames, c_totalFrames, incoming.data());
    Assert(
        ring->TryWriteFrames(generation, initial.data(), c_totalFrames, initial.size()) == PcmRingWriteResult::Written,
        "Core RX periods are fully prebuffered before capture",
        assertions);

    QueueCaptureDescriptors(guest, c_queueEntries, c_periodBytes);
    std::atomic<bool> producerReady{};
    std::atomic<bool> begin{};
    std::atomic<PcmRingWriteResult> producerResult{PcmRingWriteResult::EmptyWrite};
    std::thread producer([&] {
        producerReady.store(true, std::memory_order_release);
        while (!begin.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        producerResult.store(ring->TryWriteFrames(generation, incoming.data(), c_totalFrames, incoming.size()), std::memory_order_release);
    });
    while (!producerReady.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    begin.store(true, std::memory_order_release);
    const int completedFrames = device->PumpCapture(static_cast<int>(c_totalFrames));
    producer.join();

    Assert(
        producerResult.load(std::memory_order_acquire) == PcmRingWriteResult::Written,
        "Concurrent producer publishes its batch while core capture pumps",
        assertions);
    Assert(
        completedFrames == static_cast<int>(c_totalFrames),
        "Core capture completes all prebuffered frames during producer activity",
        assertions);
    Assert(
        Get(guest, 0x12000 + 2, 2) == c_queueEntries && Get(guest, 0x13000 + 2, 2) == c_queueEntries,
        "All RX descriptors publish used entries",
        assertions);

    const std::uint32_t stride = c_periodBytes + 8;
    for (std::uint16_t i = 0; i < c_queueEntries; ++i)
    {
        const std::uint64_t payloadAddress = 0x30000 + static_cast<std::uint64_t>(i) * stride;
        const std::uint32_t expectedSequence = static_cast<std::uint32_t>(i) * c_periodFrames;
        std::array<std::uint8_t, 8> expected{};
        for (std::uint32_t frame = 0; frame < 2; ++frame)
        {
            EncodeStereoRamp(expectedSequence + frame, expected.data() + static_cast<std::size_t>(frame) * 4);
        }
        Assert(
            std::equal(expected.begin(), expected.end(), guest.memory.begin() + static_cast<std::ptrdiff_t>(payloadAddress)),
            "Core delivers the oldest interleaved producer frames in order",
            assertions);
        Assert(
            Get(guest, payloadAddress + c_periodBytes, 4) == 0x8000,
            "Producer overlap does not turn available capture into IO_ERR/XRUN",
            assertions);
    }

    const DiagnosticSnapshot diagnostics = device->GetDiagnosticsSnapshot();
    Assert(
        diagnostics.m_rxDataCompletions == c_queueEntries && diagnostics.m_rxStatusOnlyCompletions == 0,
        "Capture pump reports data completions and no status-only XRUNs",
        assertions);
    const auto ringStats = ring->Snapshot();
    Assert(
        ringStats.m_underflowEvents == 0 && ringStats.m_sequenceFailures == 0,
        "Sufficient prebuffer distinguishes concurrency from genuine underflow or index faults",
        assertions);
}

void TestCoreUnderflowRemainsAnError(int& assertions)
{
    constexpr std::uint32_t c_periodFrames = 256;
    constexpr std::uint32_t c_periodBytes = c_periodFrames * 4;
    FakeGuest guest;
    auto ring = std::make_shared<S16LePcmRingSource>(PcmFormat{2, 48000}, 1024);
    auto device = CreateDevice(guest, ring);
    SetUpDevice(guest, *device, 4096, c_periodBytes, assertions);
    QueueCaptureDescriptors(guest, 1, c_periodBytes);

    Assert(device->PumpCapture(static_cast<int>(c_periodFrames)) == 0, "A genuine empty-ring underflow produces no data frames", assertions);
    Assert(
        Get(guest, 0x30000 + c_periodBytes, 4) == 0x8003 && Get(guest, 0x13000 + 8, 4) == 8,
        "Genuine source underflow remains an IO_ERR status-only completion",
        assertions);
    const DiagnosticSnapshot diagnostics = device->GetDiagnosticsSnapshot();
    Assert(
        diagnostics.m_rxStatusOnlyCompletions == 1 && diagnostics.m_rxDataCompletions == 0 && ring->Snapshot().m_underflowFramesMissing == c_periodFrames,
        "Underflow counters identify missing frames separately from producer contention",
        assertions);
}

void TestVirtualClockedRingAndSchedulerWithRxCredits(int& assertions)
{
    using wsl::virtio_sound::process_host::detail::CaptureEventScheduler;

    constexpr std::uint32_t c_sampleRate = 48000;
    constexpr std::uint32_t c_framesPerPacket = 480;
    constexpr std::uint32_t c_periodFrames = 2048;
    constexpr std::uint32_t c_periodBytes = c_periodFrames * 4;
    constexpr std::uint32_t c_bufferFrames = c_periodFrames * 4;
    constexpr std::uint32_t c_queueEntries = VirtioSoundDevice::c_maxQueueSize;
    constexpr std::uint32_t c_rxBuffers = 4;
    constexpr std::uint32_t c_packetCount = 6016; // 60.16 seconds, an exact multiple of the guest period.
    constexpr std::uint32_t c_totalFrames = c_packetCount * c_framesPerPacket;
    constexpr std::uint64_t c_hostStallStartMs = 5000;
    constexpr std::uint64_t c_hostStallEndMs = c_hostStallStartMs + 200;
    constexpr std::uint64_t c_consumerPauseStartMs = 30000;
    // Exceed five periods even at the least favorable completion phase, so
    // four held RX buffers are exhausted before the guest resumes.
    constexpr std::uint64_t c_consumerPauseEndMs = c_consumerPauseStartMs + 300;
    constexpr std::uint64_t c_lastPacketMs = static_cast<std::uint64_t>(c_packetCount) * 10;
    constexpr std::uint64_t c_maxDrainMs = c_lastPacketMs + 5000;
    constexpr std::uint32_t c_stride = c_periodBytes + 8;
    constexpr std::uint64_t c_ringCapacityFrames = 65536;

    static_assert(c_totalFrames % c_periodFrames == 0);
    static_assert(c_sampleRate / 100 == c_framesPerPacket);

    FakeGuest guest;
    auto ring = std::make_shared<S16LePcmRingSource>(PcmFormat{2, c_sampleRate}, c_ringCapacityFrames);
    auto source = std::make_shared<ClockedRingSource>(ring);
    auto device = CreateDevice(guest, source);
    SetUpDevice(guest, *device, c_bufferFrames * 4, c_periodBytes, assertions);
    QueueCaptureDescriptors(guest, static_cast<std::uint16_t>(c_rxBuffers), c_periodBytes);

    CaptureEventScheduler scheduler;
    std::uint64_t generation = device->CapturePacingGeneration();
    std::uint64_t lastServiceMs = 0;
    std::uint16_t nextAvailableIndex = static_cast<std::uint16_t>(c_rxBuffers);
    std::uint16_t observedUsedIndex = 0;
    std::uint64_t expectedDeliveredFrame = 0;
    std::uint64_t dataEventSignals = 0;
    std::uint64_t servicedDataEvents = 0;
    std::uint64_t serviceCalls = 0;
    std::uint64_t completedPeriods = 0;
    std::uint64_t rxWaitCalls = 0;
    std::uint64_t deferredDataWakeMilliseconds = 0;
    std::uint64_t maxFramesFromOneService = 0;
    std::size_t maxQueueDuringHostStall = 0;
    bool sourceDataEventPending = false;
    bool sawRxStarvation = false;
    std::vector<std::uint16_t> consumerHeldBuffers;
    std::vector<std::uint8_t> packet(static_cast<std::size_t>(c_framesPerPacket) * 4);

    auto completeAndCheck = [&](const wsl::virtio_sound::native::CaptureServiceResult& result, std::uint64_t nowMs) {
        Assert(result.m_frames >= 0, "Capture services never report a negative frame count", assertions);
        Assert(
            result.m_frames <= static_cast<int>(c_periodFrames),
            "One scheduler service never completes more than one negotiated period",
            assertions);
        maxFramesFromOneService = (std::max)(maxFramesFromOneService, static_cast<std::uint64_t>(result.m_frames));
        if (result.m_waitReason == wsl::virtio_sound::native::CaptureWaitReason::RxBuffer)
        {
            ++rxWaitCalls;
            sawRxStarvation = true;
        }
        if (result.m_frames == 0)
        {
            return;
        }

        Assert(result.m_frames == static_cast<int>(c_periodFrames), "Clocked capture completes whole negotiated periods", assertions);
        const std::uint16_t guestUsedIndex = static_cast<std::uint16_t>(Get(guest, 0x13000 + 2, 2));
        Assert(
            guestUsedIndex == static_cast<std::uint16_t>(observedUsedIndex + 1), "Each period publishes exactly one RX used entry", assertions);
        const std::uint64_t usedEntry = 0x13000 + 4 + 8ULL * (observedUsedIndex % c_queueEntries);
        const std::uint16_t descriptorHead = static_cast<std::uint16_t>(Get(guest, usedEntry, 4));
        const std::uint32_t usedLength = static_cast<std::uint32_t>(Get(guest, usedEntry + 4, 4));
        Assert((descriptorHead & 1U) == 0 && descriptorHead < c_rxBuffers * 2, "Used RX entry names one of the four guest buffers", assertions);
        const std::uint16_t bufferSlot = static_cast<std::uint16_t>(descriptorHead / 2);
        const std::uint64_t payloadAddress = 0x30000 + static_cast<std::uint64_t>(bufferSlot) * c_stride;
        Assert(usedLength == c_stride, "A successful RX completion reports payload plus status", assertions);
        Assert(Get(guest, payloadAddress + c_periodBytes, 4) == 0x8000, "Every delivered period has VIRTIO_SND_S_OK status", assertions);

        bool sequenceMatches = true;
        for (std::uint32_t frame = 0; frame < c_periodFrames; ++frame)
        {
            const std::uint64_t at = payloadAddress + static_cast<std::uint64_t>(frame) * 4;
            const std::uint32_t left = static_cast<std::uint32_t>(guest.memory[static_cast<std::size_t>(at)]) |
                                       (static_cast<std::uint32_t>(guest.memory[static_cast<std::size_t>(at + 1)]) << 8);
            const std::uint32_t right = static_cast<std::uint32_t>(guest.memory[static_cast<std::size_t>(at + 2)]) |
                                        (static_cast<std::uint32_t>(guest.memory[static_cast<std::size_t>(at + 3)]) << 8);
            const std::uint32_t decodedSequence = left | (right << 16);
            if (decodedSequence != static_cast<std::uint32_t>(expectedDeliveredFrame + frame))
            {
                sequenceMatches = false;
                break;
            }
        }
        Assert(sequenceMatches, "Every delivered stereo frame is ordered, unique, and from the producer ramp", assertions);

        expectedDeliveredFrame += c_periodFrames;
        ++completedPeriods;
        observedUsedIndex = guestUsedIndex;
        if (nowMs >= c_consumerPauseStartMs && nowMs < c_consumerPauseEndMs)
        {
            consumerHeldBuffers.push_back(bufferSlot);
        }
        else
        {
            RepostCaptureDescriptor(guest, bufferSlot, nextAvailableIndex, c_periodBytes);
        }
    };

    auto serviceIfWoken = [&](std::uint64_t nowMs, bool urgentGuestWake) {
        const std::uint64_t nowNs = nowMs * 1'000'000;
        const bool dataEventIncluded = sourceDataEventPending && !scheduler.DeferSourceData(nowNs);
        const bool deadlineDue = scheduler.WaitTimeoutMilliseconds(nowNs) == 0;
        if (!urgentGuestWake && !dataEventIncluded && !deadlineDue)
        {
            return false;
        }

        const bool allowTransfer = scheduler.CanTransfer(nowNs, generation);
        const std::uint64_t elapsedNs = (nowMs - lastServiceMs) * 1'000'000;
        const auto result = device->ServiceCapture(elapsedNs, allowTransfer);
        ++serviceCalls;
        lastServiceMs = nowMs;
        generation = result.m_generation;
        scheduler.OnService(nowNs, result);
        if (dataEventIncluded)
        {
            sourceDataEventPending = false;
            ++servicedDataEvents;
        }
        completeAndCheck(result, nowMs);
        return true;
    };

    for (std::uint64_t nowMs = 1; nowMs <= c_lastPacketMs; ++nowMs)
    {
        if (nowMs % 10 == 0 && nowMs / 10 <= c_packetCount)
        {
            const std::uint32_t firstFrame = static_cast<std::uint32_t>((nowMs / 10 - 1) * c_framesPerPacket);
            for (std::uint32_t frame = 0; frame < c_framesPerPacket; ++frame)
            {
                EncodeSequenceFrame(firstFrame + frame, packet.data() + static_cast<std::size_t>(frame) * 4);
            }
            Assert(
                ring->TryWriteFrames(ring->Generation(), packet.data(), c_framesPerPacket, packet.size()) == PcmRingWriteResult::Written,
                "The virtual hardware source publishes each 10 ms packet without overflow",
                assertions);
            sourceDataEventPending = true; // Auto-reset SourceData signals coalesce while the host excludes this handle.
            ++dataEventSignals;
        }

        const auto queued = ring->Snapshot().m_queuedFrames;
        if (nowMs >= c_hostStallStartMs && nowMs < c_hostStallEndMs)
        {
            maxQueueDuringHostStall = (std::max)(maxQueueDuringHostStall, queued);
            continue; // Simulate a bounded 200 ms host scheduling stall without advancing PCM by wall time.
        }

        bool urgentGuestWake = false;
        if (nowMs == c_consumerPauseEndMs)
        {
            for (const auto slot : consumerHeldBuffers)
            {
                RepostCaptureDescriptor(guest, slot, nextAvailableIndex, c_periodBytes);
            }
            urgentGuestWake = !consumerHeldBuffers.empty();
            consumerHeldBuffers.clear();
        }

        const std::uint64_t nowNs = nowMs * 1'000'000;
        if (sourceDataEventPending && scheduler.DeferSourceData(nowNs))
        {
            ++deferredDataWakeMilliseconds;
        }
        (void)serviceIfWoken(nowMs, urgentGuestWake);
    }

    for (std::uint64_t nowMs = c_lastPacketMs + 1; nowMs <= c_maxDrainMs && ring->Snapshot().m_queuedFrames != 0; ++nowMs)
    {
        (void)serviceIfWoken(nowMs, false);
    }

    const auto finalRing = ring->Snapshot();
    const auto diagnostics = device->GetDiagnosticsSnapshot();
    Assert(c_lastPacketMs >= 60000, "The virtual scenario runs for at least sixty seconds of source time", assertions);
    Assert(maxQueueDuringHostStall >= 20 * c_framesPerPacket, "The 200 ms host stall remains buffered in the source ring", assertions);
    Assert(deferredDataWakeMilliseconds != 0 && servicedDataEvents < dataEventSignals, "Armed deadlines coalesce SourceData wakes", assertions);
    Assert(sawRxStarvation && rxWaitCalls != 0, "The bounded guest consumer pause reaches the real no-RX wait path", assertions);
    Assert(maxFramesFromOneService <= c_periodFrames, "The source backlog never causes multi-period completion bursts", assertions);
    Assert(
        expectedDeliveredFrame == c_totalFrames && finalRing.m_queuedFrames == 0,
        "All produced source frames are delivered exactly once",
        assertions);
    Assert(
        finalRing.m_framesWritten == c_totalFrames && finalRing.m_framesRead == c_totalFrames,
        "Ring accounting has no lost or invented frames",
        assertions);
    Assert(
        finalRing.m_overflowEvents == 0 && finalRing.m_underflowEvents == 0 && finalRing.m_sequenceFailures == 0,
        "Virtual stalls and RX starvation produce neither overflow nor XRUN/underflow",
        assertions);
    Assert(
        diagnostics.m_rxDataCompletions == completedPeriods && diagnostics.m_rxStatusOnlyCompletions == 0,
        "Every completed period is successful data and no false IO_ERR/XRUN is reported",
        assertions);
    Assert(
        diagnostics.m_captureMaximumRxOutstandingAvailable <= c_rxBuffers,
        "The guest never exposes more RX credits than its four-period capture buffer",
        assertions);
    Assert(
        static_cast<std::uint64_t>(observedUsedIndex) == completedPeriods && serviceCalls != 0,
        "Guest used-ring progress matches the verified period count",
        assertions);
}

} // namespace

int main()
{
    try
    {
        int assertions = 0;
        TestConcurrentProducerAndCorePump(assertions);
        TestCoreUnderflowRemainsAnError(assertions);
        TestSourceArrivalBetweenServiceSnapshots(0, assertions);
        TestSourceArrivalBetweenServiceSnapshots(2047, assertions);
        TestVirtualClockedRingAndSchedulerWithRxCredits(assertions);
        std::cout << "PASS: ring/core integration "
                  << assertions << " assertions; virtual paced capture survives host/RX pauses, concurrent capture has no false XRUN, true underflow remains IO_ERR.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
