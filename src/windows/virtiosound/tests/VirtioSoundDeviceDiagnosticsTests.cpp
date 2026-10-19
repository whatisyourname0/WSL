// Copyright (C) Microsoft Corporation. All rights reserved.

#include "VirtioSoundDevice.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace wsl::virtio_sound::native;

namespace {
    struct FakeGuest
    {
        std::vector<std::uint8_t> m_memory = std::vector<std::uint8_t>(1U << 20);
    };

    class BurstSource final : public PcmSource
    {
    public:
        bool ReadFrames(std::uint32_t frames, std::uint8_t* destination, std::size_t byteCount) noexcept override
        {
            ++m_readCalls;
            if (destination == nullptr || byteCount != static_cast<std::size_t>(frames) * 4)
            {
                return false;
            }
            std::fill_n(destination, byteCount, static_cast<std::uint8_t>(0x5a));
            return true;
        }

        void Reset() noexcept override
        {
            ++m_resetCalls;
        }

        std::optional<std::uint32_t> AvailableFrames() const noexcept override
        {
            return m_availableFrames;
        }

        std::uint32_t m_availableFrames{64};
        int m_readCalls{};
        int m_resetCalls{};
    };

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool ReadGuest(void* context, std::uint64_t address, std::uint8_t* destination, std::size_t byteCount)
    {
        auto& guest = *static_cast<FakeGuest*>(context);
        if (address > guest.m_memory.size() || byteCount > guest.m_memory.size() - static_cast<std::size_t>(address))
        {
            return false;
        }
        if (byteCount != 0)
        {
            std::copy_n(guest.m_memory.data() + static_cast<std::size_t>(address), byteCount, destination);
        }
        return true;
    }

    bool WriteGuest(void* context, std::uint64_t address, const std::uint8_t* source, std::size_t byteCount)
    {
        auto& guest = *static_cast<FakeGuest*>(context);
        if (address > guest.m_memory.size() || byteCount > guest.m_memory.size() - static_cast<std::size_t>(address))
        {
            return false;
        }
        if (byteCount != 0)
        {
            std::copy_n(source, byteCount, guest.m_memory.data() + static_cast<std::size_t>(address));
        }
        return true;
    }

    bool DeliverMsi(void*, std::uint64_t, std::uint32_t)
    {
        return true;
    }

    void Put(FakeGuest& guest, std::uint64_t address, std::uint64_t value, std::size_t byteCount)
    {
        for (std::size_t i = 0; i < byteCount; ++i)
        {
            guest.m_memory[static_cast<std::size_t>(address) + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }

    std::uint64_t Get(const FakeGuest& guest, std::uint64_t address, std::size_t byteCount)
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < byteCount; ++i)
        {
            value |= static_cast<std::uint64_t>(guest.m_memory[static_cast<std::size_t>(address) + i]) << (8 * i);
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

    void ConfigureQueue(VirtioSoundDevice& device, std::uint16_t index, std::uint64_t descriptor, std::uint64_t available, std::uint64_t used)
    {
        BarWrite(device, 22, index, 2);
        BarWrite(device, 26, 0xffff, 2);
        BarWrite(device, 32, descriptor, 8);
        BarWrite(device, 40, available, 8);
        BarWrite(device, 48, used, 8);
        BarWrite(device, 28, 1, 2);
    }

    void PutDescriptor(
        FakeGuest& guest,
        std::uint64_t table,
        std::uint16_t index,
        std::uint64_t address,
        std::uint32_t length,
        std::uint16_t flags,
        std::uint16_t next)
    {
        const std::uint64_t descriptor = table + 16ULL * index;
        Put(guest, descriptor, address, 8);
        Put(guest, descriptor + 8, length, 4);
        Put(guest, descriptor + 12, flags, 2);
        Put(guest, descriptor + 14, next, 2);
    }

    void SubmitControl(FakeGuest& guest, VirtioSoundDevice& device, std::uint16_t availableIndex, const std::uint8_t* request, std::size_t requestBytes,
        std::uint32_t expectedStatus = 0x8000)
    {
        std::copy_n(request, requestBytes, guest.m_memory.data() + 0x4000);
        PutDescriptor(guest, 0x1000, 0, 0x4000, static_cast<std::uint32_t>(requestBytes), 1, 1);
        PutDescriptor(guest, 0x1000, 1, 0x5000, 4, 2, 0);
        const std::uint16_t slot = static_cast<std::uint16_t>((availableIndex - 1) % 128);
        Put(guest, 0x2000 + 4ULL + 2ULL * slot, 0, 2);
        Put(guest, 0x2002, availableIndex, 2);
        Require(device.ProcessControlQueue() == 1, "PCM control command completes");
        Require(Get(guest, 0x5000, 4) == expectedStatus, "PCM control command returns expected status");
    }

    void PublishCapture(FakeGuest& guest, std::uint16_t ringSlot, std::uint16_t availableIndex, std::uint16_t head, std::uint64_t request, std::uint64_t payload)
    {
        Put(guest, request, 0, 4);
        PutDescriptor(guest, 0x7000, head, request, 4, 1, static_cast<std::uint16_t>(head + 1));
        PutDescriptor(guest, 0x7000, static_cast<std::uint16_t>(head + 1), payload, 72, 2, 0);
        Put(guest, 0x8000 + 4ULL + 2ULL * ringSlot, head, 2);
        Put(guest, 0x8002, availableIndex, 2);
    }

    void TestCaptureDiagnostics()
    {
        FakeGuest guest;
        auto source = std::make_shared<BurstSource>();
        GuestMemoryCallbacks memory{&guest, &ReadGuest, &WriteGuest};
        InterruptCallbacks interrupts{nullptr, &DeliverMsi, nullptr};
        VirtioSoundDevice device(memory, interrupts, source);

        const DiagnosticSnapshot initial = device.GetDiagnosticsSnapshot();
        Require(initial.m_capturePumpCalls == 0 && initial.m_capturePumpCompletedFrames == 0, "Capture counters start at zero");
        Require(initial.m_captureScratchCapacityGrowths == 0 && initial.m_captureScratchCapacityBytes == 0,
            "An unconfigured device does not allocate PCM scratch");

        Require(device.PumpCaptureElapsed(1234) == 0, "Elapsed time is ignored before stream setup");
        Require(device.PumpCaptureElapsed(2345) == 0, "A second elapsed interval is ignored before stream setup");

        Negotiate(device);
        ConfigureQueue(device, 0, 0x1000, 0x2000, 0x3000);
        ConfigureQueue(device, 3, 0x7000, 0x8000, 0x9000);
        std::array<std::uint8_t, 24> setParams{};
        setParams[0] = 1;
        setParams[1] = 1;
        setParams[9] = 0x10; // 4096-byte guest buffer
        setParams[12] = 0x40;
        setParams[20] = 2;
        setParams[21] = 5;
        setParams[22] = 7;
        auto invalidParams = setParams;
        invalidParams[12] = 0; // A zero period must not allocate storage.
        SubmitControl(guest, device, 1, invalidParams.data(), invalidParams.size(), 0x8001);
        Require(device.GetDiagnosticsSnapshot().m_captureScratchCapacityBytes == 0,
            "Invalid configuration does not allocate PCM scratch");
        SubmitControl(guest, device, 2, setParams.data(), setParams.size());
        const DiagnosticSnapshot configured = device.GetDiagnosticsSnapshot();
        Require(configured.m_captureScratchCapacityGrowths == 1 &&
            configured.m_captureScratchCapacityBytes >= VirtioSoundDevice::c_maxTransferBytes - 12,
            "Valid configuration allocates the full legal PCM bound before START");
        std::array<std::uint8_t, 8> command{};
        command[0] = 2;
        command[1] = 1;
        SubmitControl(guest, device, 3, command.data(), command.size());
        command[0] = 4;
        SubmitControl(guest, device, 4, command.data(), command.size());

        PublishCapture(guest, 0, 1, 0, 0xa000, 0xb000);
        PublishCapture(guest, 1, 2, 2, 0xa100, 0xb100);
        Require(device.PumpCapture(0) == 0, "A zero frame budget keeps the first capture chain pending");
        Require(device.PumpCapture(32) == 32, "A later pump completes both capture periods");
        Require(source->m_readCalls == 2, "Both PCM periods reach the source");
        Require(
            std::all_of(guest.m_memory.begin() + 0xb000, guest.m_memory.begin() + 0xb040, [](std::uint8_t value) { return value == 0x5a; }),
            "First scratch copy reaches guest memory intact");
        Require(
            std::all_of(guest.m_memory.begin() + 0xb100, guest.m_memory.begin() + 0xb140, [](std::uint8_t value) { return value == 0x5a; }),
            "Second scratch copy reaches guest memory intact");

        const DiagnosticSnapshot after = device.GetDiagnosticsSnapshot();
        Require(after.m_captureElapsedPumpCalls == 2, "Elapsed pump calls are counted");
        Require(after.m_captureElapsedNanoseconds == 3579, "Elapsed nanoseconds accumulate exactly");
        Require(after.m_captureMaximumElapsedNanoseconds == 2345, "Maximum elapsed interval records the high-water value");
        Require(after.m_capturePumpCalls == 2, "Direct capture pump calls are counted");
        Require(after.m_capturePumpRequestedFrames == 32, "Requested capture frames accumulate");
        Require(after.m_capturePumpCompletedFrames == 32, "Completed capture frames accumulate");
        Require(after.m_capturePumpNoProgressCalls == 1, "A pump with no completed frames is counted");
        Require(after.m_captureMaximumRxOutstandingAvailable == 2, "RX descriptor backlog high-water is recorded");
        Require(after.m_captureMaximumSourceAvailableFrames == 64, "Source frame backlog high-water is recorded");
        Require(after.m_captureScratchCapacityGrowths == 1, "Repeated periods do not grow the scratch allocation");
        Require(
            after.m_captureScratchCapacityBytes == configured.m_captureScratchCapacityBytes,
            "Scratch capacity stays fixed while periods are copied");

        // Only active ServiceCapture calls sample the queue. Direct pumps and
        // idle time must not inflate this event-weighted distribution.
        for (const auto frames : {0U, 1U, 16U, 32U, 64U, 128U})
        {
            source->m_availableFrames = frames;
            const auto service = device.ServiceCapture(0, false);
            Require(service.m_sourceAvailableFrames == frames, "Service exposes post-service queue occupancy");
        }
        const auto sampled = device.GetDiagnosticsSnapshot();
        for (const auto count : sampled.m_captureBacklogSamples)
        {
            Require(count == 1, "Each backlog bucket has exactly one active service sample");
        }
        PublishCapture(guest, 2, 3, 4, 0xa200, 0xb200);
        PublishCapture(guest, 3, 4, 6, 0xa300, 0xb300);
        command[0] = 5; // STOP
        SubmitControl(guest, device, 5, command.data(), command.size());
        (void)device.ServiceCapture(1'000'000'000, false);
        command[0] = 3; // RELEASE returns unused buffers, without a capture fault.
        SubmitControl(guest, device, 6, command.data(), command.size());
        const auto released = device.GetDiagnosticsSnapshot();
        Require(
            released.m_captureBacklogSamples == sampled.m_captureBacklogSamples,
            "Idle service does not sample stale source frames");
        Require(
            released.m_captureReleaseCompletions == 2 && released.m_captureErrorCompletions == 0,
            "Expected release completions are separate from capture errors");
        Require(
            Get(guest, 0xb240, 4) == 0x8003 && Get(guest, 0xb340, 4) == 0x8003,
            "Diagnostic classification does not change RELEASE wire status");

        device.Reset();
        const DiagnosticSnapshot afterReset = device.GetDiagnosticsSnapshot();
        Require(afterReset.m_capturePumpCompletedFrames == 32, "Lifetime capture totals survive a device reset");
        Require(afterReset.m_captureScratchCapacityGrowths == 1, "Reset retains the bounded scratch storage");
        Require(
            afterReset.m_captureScratchCapacityBytes == configured.m_captureScratchCapacityBytes,
            "Reset retains the same scratch capacity");
        Negotiate(device);
        ConfigureQueue(device, 0, 0x1000, 0x2000, 0x3000);
        SubmitControl(guest, device, 1, setParams.data(), setParams.size());
        const DiagnosticSnapshot reconfigured = device.GetDiagnosticsSnapshot();
        Require(reconfigured.m_captureScratchCapacityGrowths == 1 &&
            reconfigured.m_captureScratchCapacityBytes == configured.m_captureScratchCapacityBytes,
            "Reconfiguration reuses scratch without allocating on the capture path");
    }
} // namespace

int main()
{
    try
    {
        TestCaptureDiagnostics();
        std::cout << "PASS: core capture diagnostics and bounded scratch reuse\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
