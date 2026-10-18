// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#ifndef _WIN32
#error AudioPipeTransport is available only on Windows.
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <vector>

#include "AudioIpcCodec.hpp"

namespace wsl::virtio_sound::audio_ipc::windows {

enum class PipeResult
{
    Success,
    Timeout,
    Cancelled,
    Disconnected,
    InvalidMessage,
    IoError,
};

// Read exactly one bounded wire message from a connected byte-mode named pipe
// opened with FILE_FLAG_OVERLAPPED. The header is validated before payload
// allocation and the whole read shares one deadline. `out` is unchanged
// unless the complete message validates.
// `stopEvent` may be null. Callers must serialize reads on a pipe; one read and
// one write may run concurrently because each operation owns its OVERLAPPED
// event. Treat Timeout or Cancelled as terminal for this byte stream because
// part of a frame may already have been consumed.
// A timeout bounds the normal operation wait; cancellation must still drain
// the kernel operation before its OVERLAPPED storage can be released, so this
// API does not promise a hard wall-clock return bound under an OS stall.
PipeResult ReadMessage(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, Message& out) noexcept;

// Synchronous writer for one serialized pipe session. It retains a bounded
// wire buffer and one manual-reset event between writes. Each Write call
// completes or cancels and drains its overlapped operation before returning,
// so the event and encoded bytes are safe to reuse on the next call. Calls on
// one instance must be serialized. Timeout or cancellation can leave a partial
// frame in the byte stream; discard that pipe before retrying with this writer.
class PipeMessageWriter final
{
public:
    PipeMessageWriter() noexcept = default;
    ~PipeMessageWriter() noexcept;

    PipeMessageWriter(const PipeMessageWriter&) = delete;
    PipeMessageWriter& operator=(const PipeMessageWriter&) = delete;
    PipeMessageWriter(PipeMessageWriter&&) = delete;
    PipeMessageWriter& operator=(PipeMessageWriter&&) = delete;

    PipeResult Write(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, const Message& message) noexcept;

private:
    HANDLE m_operationEvent{};
    std::vector<std::uint8_t> m_wire;
};

// Validate and write one complete bounded wire message to a connected
// byte-mode pipe opened with FILE_FLAG_OVERLAPPED, using exact partial writes
// under one total deadline. `stopEvent` may be null. Callers must serialize
// writes on a pipe so separate messages cannot interleave. Timeout or
// cancellation can leave a partial frame; discard that pipe before retrying.
// Cancellation drains kernel completion and therefore has no hard wall-clock
// return guarantee if the operating system does not complete the request.
PipeResult WriteMessage(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, const Message& message) noexcept;

} // namespace wsl::virtio_sound::audio_ipc::windows
