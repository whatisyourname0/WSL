// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace wsl::virtio_sound::native::windows::detail {

// Portable lifecycle policy for the optional RX kick doorbell. This helper is
// intentionally independent of Windows types so fake callbacks can exercise
// its ownership rules without loading vmdevicehost.dll.
struct RxDoorbellSpecification final
{
    static constexpr std::uint32_t c_barIndex = 0; // HDV_PCI_BAR0
    static constexpr std::uint64_t c_barOffset = 0x30c;
    static constexpr std::uint64_t c_triggerValue = 3; // virtio-sound queue 3
    static constexpr std::uint64_t c_flags = 2; // HDV_DOORBELL_FLAG_TRIGGER_SIZE_WORD
};

enum class RxDoorbellState : std::uint8_t
{
    Unattempted,
    Registering,
    Disabled,
    RegistrationFailed,
    Registered,
    Unregistering,
    Unregistered,
    UnregisterFailed,
    Prepared,
    UnregisterSkippedAfterGuestStop,
    Retired
};

class RxDoorbellLifecycle final
{
public:
    template <typename Register>
    bool TryRegister(bool apiPairAvailable, bool eventAvailable, Register&& registerDoorbell) noexcept
    {
        {
            std::lock_guard lock{m_mutex};
            // Registration is one-shot, and a pre-stop request permanently
            // closes admission even when no doorbell was active yet.
            if (m_stopRequested || m_state != RxDoorbellState::Unattempted)
            {
                return false;
            }
            m_state = RxDoorbellState::Registering;
        }
        if (!apiPairAvailable || !eventAvailable)
        {
            {
                std::lock_guard lock{m_mutex};
                m_state = RxDoorbellState::Disabled;
            }
            m_changed.notify_all();
            return false;
        }

        bool registered{};
        try
        {
            registered = registerDoorbell(RxDoorbellSpecification{});
        }
        catch (...)
        {
            registered = false;
        }
        {
            std::lock_guard lock{m_mutex};
            m_state = registered ? RxDoorbellState::Registered : RxDoorbellState::RegistrationFailed;
        }
        m_changed.notify_all();
        return registered;
    }

    template <typename Unregister>
    bool TryUnregister(Unregister&& unregisterDoorbell) noexcept
    {
        {
            std::unique_lock lock{m_mutex};
            m_changed.wait(
                lock, [this] { return m_state != RxDoorbellState::Registering && m_state != RxDoorbellState::Unregistering; });
            if (m_state != RxDoorbellState::Registered)
            {
                return !MustRetainEventLocked();
            }
            m_state = RxDoorbellState::Unregistering;
        }

        const bool unregistered = InvokeUnregister(std::forward<Unregister>(unregisterDoorbell));
        {
            std::lock_guard lock{m_mutex};
            m_state = unregistered ? RxDoorbellState::Unregistered : RxDoorbellState::UnregisterFailed;
        }
        m_changed.notify_all();
        return unregistered;
    }

    // Called on the host pump thread before orderly guest shutdown. Registration
    // admission closes first; an in-flight API call is drained before deciding
    // whether an unregister is required. No callback is disarmed here.
    template <typename Unregister>
    bool PrepareForGuestStop(Unregister&& unregisterDoorbell) noexcept
    {
        {
            std::unique_lock lock{m_mutex};
            m_stopRequested = true;
            m_changed.wait(
                lock, [this] { return m_state != RxDoorbellState::Registering && m_state != RxDoorbellState::Unregistering; });
            switch (m_state)
            {
            case RxDoorbellState::Unattempted:
            case RxDoorbellState::Disabled:
            case RxDoorbellState::RegistrationFailed:
            case RxDoorbellState::Unregistered:
            case RxDoorbellState::Prepared:
                m_state = RxDoorbellState::Prepared;
                return true;
            case RxDoorbellState::Registered:
                m_state = RxDoorbellState::Unregistering;
                break;
            default:
                return false;
            }
        }

        const bool unregistered = InvokeUnregister(std::forward<Unregister>(unregisterDoorbell));
        {
            std::lock_guard lock{m_mutex};
            m_state = unregistered ? RxDoorbellState::Prepared : RxDoorbellState::UnregisterFailed;
        }
        m_changed.notify_all();
        return unregistered;
    }

    // This is called only after VM exit. Never invoke HDV unregister here:
    // if preparation did not remove an active doorbell, retain its callback
    // root and wake event rather than making an invalid post-exit API call.
    bool CompleteAfterGuestStop() noexcept
    {
        std::unique_lock lock{m_mutex};
        m_stopRequested = true;
        m_changed.wait(
            lock, [this] { return m_state != RxDoorbellState::Registering && m_state != RxDoorbellState::Unregistering; });
        if (m_state == RxDoorbellState::Registered)
        {
            m_state = RxDoorbellState::UnregisterSkippedAfterGuestStop;
            return false;
        }
        if (m_state == RxDoorbellState::UnregisterFailed || m_state == RxDoorbellState::UnregisterSkippedAfterGuestStop)
        {
            return false;
        }
        m_state = RxDoorbellState::Retired;
        return true;
    }

    RxDoorbellState State() const noexcept
    {
        std::lock_guard lock{m_mutex};
        return m_state;
    }
    bool Active() const noexcept { return State() == RxDoorbellState::Registered; }

    // If unregister fails, the event may still be referenced by HDV. Keep the
    // BackendState pinned so its shared CaptureWakeSignal cannot close.
    bool MustRetainEvent() const noexcept
    {
        std::lock_guard lock{m_mutex};
        return MustRetainEventLocked();
    }

private:
    template <typename Unregister>
    static bool InvokeUnregister(Unregister&& unregisterDoorbell) noexcept
    {
        try
        {
            return unregisterDoorbell(RxDoorbellSpecification{});
        }
        catch (...)
        {
            return false;
        }
    }

    bool MustRetainEventLocked() const noexcept
    {
        return m_state == RxDoorbellState::Registering || m_state == RxDoorbellState::Registered ||
               m_state == RxDoorbellState::Unregistering || m_state == RxDoorbellState::UnregisterFailed ||
               m_state == RxDoorbellState::UnregisterSkippedAfterGuestStop;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    RxDoorbellState m_state{RxDoorbellState::Unattempted};
    bool m_stopRequested{};
};

} // namespace wsl::virtio_sound::native::windows::detail
