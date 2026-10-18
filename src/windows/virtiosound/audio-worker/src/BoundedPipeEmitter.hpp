// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <array>
#include <cstddef>

#include "AudioWorkerSession.hpp"

namespace wsl::virtio_sound::audio_worker {

// The worker's outbound queue is bounded independently of the async writer.
// Its fixed slots retain wire-buffer capacity after dequeue so PCM frames can
// rotate between the session, queue, and writer without another full copy.
class BoundedPipeEmitter final : public IAudioWorkerMessageEmitter
{
public:
    static constexpr std::size_t c_maximumQueuedMessages = 2U;

    bool Emit(const audio_ipc::Message& message) noexcept override
    {
        if (Full())
        {
            return false;
        }

        audio_ipc::Error error = audio_ipc::Error::None;
        try
        {
            if (!audio_ipc::EncodeMessage(message, Tail().m_bytes, error))
            {
                return false;
            }
        }
        catch (...)
        {
            return false;
        }
        ++m_size;
        return true;
    }

    bool EmitPrepared(audio_ipc::OwnedWireMessage& message) noexcept override
    {
        if (Full())
        {
            return false;
        }

        audio_ipc::Error error = audio_ipc::Error::None;
        if (!audio_ipc::ValidateWireMessage(message.m_bytes.data(), message.m_bytes.size(), error))
        {
            return false;
        }

        Tail().m_bytes.swap(message.m_bytes);
        ++m_size;
        return true;
    }

    bool Empty() const noexcept
    {
        return m_size == 0U;
    }

    std::size_t Size() const noexcept
    {
        return m_size;
    }

    audio_ipc::OwnedWireMessage& Front() noexcept
    {
        return m_messages[m_head];
    }

    const audio_ipc::OwnedWireMessage& Front() const noexcept
    {
        return m_messages[m_head];
    }

    void PopFront() noexcept
    {
        if (m_size != 0U)
        {
            m_head = (m_head + 1U) % m_messages.size();
            --m_size;
        }
    }

private:
    bool Full() const noexcept
    {
        return m_size == m_messages.size();
    }

    audio_ipc::OwnedWireMessage& Tail() noexcept
    {
        return m_messages[(m_head + m_size) % m_messages.size()];
    }

    std::array<audio_ipc::OwnedWireMessage, c_maximumQueuedMessages> m_messages{};
    std::size_t m_head{};
    std::size_t m_size{};
};

} // namespace wsl::virtio_sound::audio_worker
