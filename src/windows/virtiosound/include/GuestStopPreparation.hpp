// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>

namespace wsl::virtio_sound::native::windows::detail {

enum class GuestStopPreparationResult : std::uint8_t
{
    Prepared,
    Rejected,
    RequestFailed,
    TimedOut,
    ChildExited,
    WaitFailed,
    AlreadyRequested
};

// Keep the small parent-side protocol policy portable so request ordering,
// bounded waits, and negative acknowledgements can be tested without HDV.
struct GuestStopPreparation final
{
    // The configured waits total 4 seconds: 2 seconds for the pump acknowledgement,
    // then 2 seconds for child exit after an emergency job close. OS scheduling
    // can extend wall-clock time. The emergency abort does not prove the doorbell
    // was unregistered.
    static constexpr std::uint32_t c_timeoutMilliseconds = 2000;
    static constexpr std::uint32_t c_abortChildWaitMilliseconds = 2000;
    static constexpr std::uint32_t c_maximumWaitMilliseconds = c_timeoutMilliseconds + c_abortChildWaitMilliseconds;

    template <typename ResetAcknowledgements, typename SignalRequest, typename WaitForReply, typename AbortChild>
    static GuestStopPreparationResult Request(
        ResetAcknowledgements&& resetAcknowledgements, SignalRequest&& signalRequest, WaitForReply&& waitForReply, AbortChild&& abortChild) noexcept
    {
        try
        {
            if (!resetAcknowledgements() || !signalRequest())
            {
                return GuestStopPreparationResult::RequestFailed;
            }
        }
        catch (...)
        {
            return GuestStopPreparationResult::RequestFailed;
        }

        GuestStopPreparationResult result{};
        try
        {
            result = waitForReply(c_timeoutMilliseconds);
        }
        catch (...)
        {
            // A wait exception happens after the request was successfully
            // signaled, so the child may still have a queued prepare request.
            result = GuestStopPreparationResult::WaitFailed;
        }

        if (result == GuestStopPreparationResult::TimedOut || result == GuestStopPreparationResult::WaitFailed)
        {
            try
            {
                abortChild(result);
            }
            catch (...)
            {
                // Keep the protocol result; production's abort callback is
                // noexcept and records its own bounded-wait outcome.
            }
        }
        return result;
    }
};

} // namespace wsl::virtio_sound::native::windows::detail
