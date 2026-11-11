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

// Shared ownership keeps the handles alive for pinned HDV callback contexts.
// The events coalesce rescan requests; they do not count frames or grant credit.
class CaptureWakeSignal final
{
public:
    CaptureWakeSignal() :
        m_event(CreateEventW(nullptr, FALSE, FALSE, nullptr)), m_dataEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr))
    {
        THROW_LAST_ERROR_IF(!m_event);
        THROW_LAST_ERROR_IF(!m_dataEvent);
    }

    void Notify(CaptureWakeReason reason) noexcept
    {
        m_reasons.fetch_or(static_cast<std::uint32_t>(reason), std::memory_order_release);
        (void)SetEvent(reason == CaptureWakeReason::SourceData ? m_dataEvent.get() : m_event.get());
    }

    // Source state and guest activity remain urgent while data-only wakeups
    // can be deferred until the scheduler's next rescan deadline.
    HANDLE Event() const noexcept { return m_event.get(); }
    HANDLE DataEvent() const noexcept
    {
        return m_dataEvent.get();
    }
    std::uint32_t Consume() noexcept { return m_reasons.exchange(0, std::memory_order_acq_rel); }

private:
    wil::unique_handle m_event;
    wil::unique_handle m_dataEvent;
    std::atomic<std::uint32_t> m_reasons{};
};
} // namespace wsl::virtio_sound::native::windows
