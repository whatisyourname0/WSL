// Copyright (C) Microsoft Corporation. All rights reserved.
//
// Offline core benchmark. Build this same source against the baseline or
// candidate VirtioSoundDevice.cpp to compare steady-state allocations and time.

#include "VirtioSoundDevice.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
    std::atomic<bool> g_countAllocations{};
    std::atomic<std::uint64_t> g_allocationCount{};

    struct FakeGuest
    {
        std::vector<std::uint8_t> m_memory = std::vector<std::uint8_t>(1U << 20);
    };

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

    void BarWrite(wsl::virtio_sound::native::VirtioSoundDevice& device, std::uint64_t offset, std::uint64_t value, std::size_t width)
    {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t i = 0; i < width; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
        device.WriteBar(offset, bytes.data(), width);
    }

    void Negotiate(wsl::virtio_sound::native::VirtioSoundDevice& device)
    {
        BarWrite(device, 0, 1, 4);
        BarWrite(device, 8, 1, 4);
        BarWrite(device, 12, 1, 4);
        BarWrite(device, 20, 1, 1);
        BarWrite(device, 20, 3, 1);
        BarWrite(device, 20, 11, 1);
        BarWrite(device, 20, 15, 1);
    }

    void ConfigureQueue(
        wsl::virtio_sound::native::VirtioSoundDevice& device,
        std::uint16_t index,
        std::uint64_t descriptor,
        std::uint64_t available,
        std::uint64_t used)
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

    void SubmitControl(
        FakeGuest& guest,
        wsl::virtio_sound::native::VirtioSoundDevice& device,
        std::uint16_t availableIndex,
        const std::uint8_t* request,
        std::size_t requestBytes)
    {
        std::copy_n(request, requestBytes, guest.m_memory.data() + 0x4000);
        PutDescriptor(guest, 0x1000, 0, 0x4000, static_cast<std::uint32_t>(requestBytes), 1, 1);
        PutDescriptor(guest, 0x1000, 1, 0x5000, 4, 2, 0);
        const std::uint16_t ringSlot = static_cast<std::uint16_t>((availableIndex - 1) % 128);
        Put(guest, 0x2000 + 4ULL + 2ULL * ringSlot, 0, 2);
        Put(guest, 0x2002, availableIndex, 2);
        if (device.ProcessControlQueue() != 1)
        {
            throw std::runtime_error("Could not configure benchmark capture stream");
        }
    }

    std::uint64_t ParseIterations(int argc, char** argv)
    {
        constexpr std::uint64_t c_defaultIterations = 20000;
        if (argc < 2)
        {
            return c_defaultIterations;
        }
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || parsed == 0 || parsed > 1000000ULL)
        {
            throw std::invalid_argument("Iterations must be in the range 1..1000000");
        }
        return static_cast<std::uint64_t>(parsed);
    }
} // namespace

void* operator new(std::size_t size)
{
    if (void* memory = std::malloc(size == 0 ? 1 : size))
    {
        if (g_countAllocations.load(std::memory_order_relaxed))
        {
            g_allocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        return memory;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

int main(int argc, char** argv)
{
    using wsl::virtio_sound::native::GuestMemoryCallbacks;
    using wsl::virtio_sound::native::InterruptCallbacks;
    using wsl::virtio_sound::native::VirtioSoundDevice;

    try
    {
        const std::uint64_t iterations = ParseIterations(argc, argv);
        FakeGuest guest;
        GuestMemoryCallbacks memory{&guest, &ReadGuest, &WriteGuest};
        InterruptCallbacks interrupts{nullptr, &DeliverMsi, nullptr};
        VirtioSoundDevice device(memory, interrupts);

        Negotiate(device);
        ConfigureQueue(device, 0, 0x1000, 0x2000, 0x3000);
        ConfigureQueue(device, 3, 0x7000, 0x8000, 0x9000);

        std::array<std::uint8_t, 24> setParams{};
        setParams[0] = 1;
        setParams[1] = 1;
        setParams[8] = 0x00;
        setParams[9] = 0x1e; // 7680-byte guest buffer
        setParams[12] = 0x80;
        setParams[13] = 0x07; // 1920-byte period
        setParams[20] = 2;
        setParams[21] = 5;
        setParams[22] = 7;
        SubmitControl(guest, device, 1, setParams.data(), setParams.size());
        std::array<std::uint8_t, 8> command{};
        command[0] = 2;
        command[1] = 1;
        SubmitControl(guest, device, 2, command.data(), command.size());
        command[0] = 4;
        SubmitControl(guest, device, 3, command.data(), command.size());

        constexpr std::uint32_t c_periodFrames = 480;
        constexpr std::uint32_t c_periodBytes = c_periodFrames * 4;
        Put(guest, 0xa000, 0, 4);
        PutDescriptor(guest, 0x7000, 0, 0xa000, 4, 1, 1);
        PutDescriptor(guest, 0x7000, 1, 0xb000, c_periodBytes + 8, 2, 0);

        const std::uint64_t allocationStart = g_allocationCount.load(std::memory_order_relaxed);
        const auto start = std::chrono::steady_clock::now();
        g_countAllocations.store(true, std::memory_order_relaxed);
        std::uint64_t completedFrames = 0;
        for (std::uint64_t i = 0; i < iterations; ++i)
        {
            const std::uint16_t ringSlot = static_cast<std::uint16_t>(i % 128);
            const std::uint16_t availableIndex = static_cast<std::uint16_t>(i + 1);
            Put(guest, 0x8000 + 4ULL + 2ULL * ringSlot, 0, 2);
            Put(guest, 0x8002, availableIndex, 2);
            const int frames = device.PumpCapture(static_cast<int>(c_periodFrames));
            if (frames != static_cast<int>(c_periodFrames))
            {
                g_countAllocations.store(false, std::memory_order_relaxed);
                throw std::runtime_error("Capture pump did not complete one full benchmark period");
            }
            completedFrames += static_cast<std::uint64_t>(frames);
        }
        const auto end = std::chrono::steady_clock::now();
        g_countAllocations.store(false, std::memory_order_relaxed);
        const std::uint64_t allocationEnd = g_allocationCount.load(std::memory_order_relaxed);
        const auto elapsedNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "iterations=" << iterations << " completed_frames=" << completedFrames
                  << " allocations=" << (allocationEnd - allocationStart) << " elapsed_ns=" << elapsedNanoseconds
                  << " ns_per_period=" << (elapsedNanoseconds / static_cast<std::int64_t>(iterations)) << '\n';
        return completedFrames == iterations * c_periodFrames ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        g_countAllocations.store(false, std::memory_order_relaxed);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
