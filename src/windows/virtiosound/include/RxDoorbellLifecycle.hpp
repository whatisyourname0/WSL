// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>

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
    UnregisterFailed
};

class RxDoorbellLifecycle final
{
public:
    template <typename Register>
    bool TryRegister(bool apiPairAvailable, bool eventAvailable, Register&& registerDoorbell) noexcept
    {
        // Each backend may attempt registration exactly once. Claim the
        // unattempted state first so a repeated/concurrent call cannot replace
        // an active binding or obscure its cleanup obligation.
        auto expected = RxDoorbellState::Unattempted;
        if (!m_state.compare_exchange_strong(expected, RxDoorbellState::Registering, std::memory_order_acq_rel))
        {
            return false;
        }
        if (!apiPairAvailable || !eventAvailable)
        {
            m_state.store(RxDoorbellState::Disabled, std::memory_order_release);
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
        m_state.store(registered ? RxDoorbellState::Registered : RxDoorbellState::RegistrationFailed, std::memory_order_release);
        return registered;
    }

    template <typename Unregister>
    bool TryUnregister(Unregister&& unregisterDoorbell) noexcept
    {
        auto expected = RxDoorbellState::Registered;
        if (!m_state.compare_exchange_strong(expected, RxDoorbellState::Unregistering, std::memory_order_acq_rel))
        {
            return !MustRetainEvent();
        }

        bool unregistered{};
        try
        {
            unregistered = unregisterDoorbell(RxDoorbellSpecification{});
        }
        catch (...)
        {
            unregistered = false;
        }
        m_state.store(unregistered ? RxDoorbellState::Unregistered : RxDoorbellState::UnregisterFailed, std::memory_order_release);
        return unregistered;
    }

    RxDoorbellState State() const noexcept { return m_state.load(std::memory_order_acquire); }
    bool Active() const noexcept { return State() == RxDoorbellState::Registered; }

    // If unregister fails, the event may still be referenced by HDV. Keep the
    // BackendState pinned so its shared CaptureWakeSignal cannot close.
    bool MustRetainEvent() const noexcept
    {
        const auto state = State();
        return state == RxDoorbellState::Registering || state == RxDoorbellState::Registered || state == RxDoorbellState::Unregistering ||
               state == RxDoorbellState::UnregisterFailed;
    }

private:
    std::atomic<RxDoorbellState> m_state{RxDoorbellState::Unattempted};
};

} // namespace wsl::virtio_sound::native::windows::detail
