// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <wil/resource.h>
#include <wil/result.h>

namespace wsl::virtio_sound::native::windows {

enum class CaptureWakeReason : std::uint32_t
{
    SourceData = 1,
    SourceState = 2,
    GuestQueue = 4,
    GuestState = 8
};

// Shared ownership keeps the handle alive for pinned HDV callback contexts.
// The event coalesces rescan requests; it does not count frames or grant credit.
class CaptureWakeSignal final
{
public:
    CaptureWakeSignal() : m_event(CreateEventW(nullptr, FALSE, FALSE, nullptr))
    {
        THROW_LAST_ERROR_IF(!m_event);
    }

    void Notify(CaptureWakeReason reason) noexcept
    {
        m_reasons.fetch_or(static_cast<std::uint32_t>(reason), std::memory_order_release);
        (void)SetEvent(m_event.get());
    }

    HANDLE Event() const noexcept { return m_event.get(); }
    std::uint32_t Consume() noexcept { return m_reasons.exchange(0, std::memory_order_acq_rel); }

private:
    wil::unique_handle m_event;
    std::atomic<std::uint32_t> m_reasons{};
};
} // namespace wsl::virtio_sound::native::windows
