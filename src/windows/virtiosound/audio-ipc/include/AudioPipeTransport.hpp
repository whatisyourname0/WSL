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
#include <array>
#include <cstddef>
#include <cstdint>
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
    Pending,
};

// Stateful overlapped reader for one message at a time. Begin starts a read for
// up to the full header and returns Pending when the request has been issued.
// Advance is nonblocking (apart from draining an expired/cancelled request) and
// is the only method that reports a completed message. The initial header read
// has no deadline; partialFrameTimeoutMs starts when its first bytes arrive and
// covers the rest of that frame. The pipe handle is borrowed and must remain
// valid until the operation completes or CancelAndDrain returns. Advance
// reports a completed message once. CancelAndDrain cancels, drains, and returns
// the reader to Idle; discard that pipe because part of a frame may already
// have been read. A timeout is also terminal for its pipe. Successful delivery
// reuses a bounded prior output payload buffer on the next frame. Calls on an
// instance must be serialized. A single read and one write may run concurrently
// on a pipe.
class PipeMessageReader final
{
public:
    PipeMessageReader() noexcept = default;
    ~PipeMessageReader() noexcept;

    PipeMessageReader(const PipeMessageReader&) = delete;
    PipeMessageReader& operator=(const PipeMessageReader&) = delete;
    PipeMessageReader(PipeMessageReader&&) = delete;
    PipeMessageReader& operator=(PipeMessageReader&&) = delete;

    PipeResult Begin(HANDLE pipe, DWORD partialFrameTimeoutMs) noexcept;
    PipeResult Advance(Message& out) noexcept;
    HANDLE Event() const noexcept;
    DWORD WaitTimeoutMilliseconds() const noexcept;
    void CancelAndDrain() noexcept;

private:
    enum class State : std::uint8_t
    {
        Idle,
        Header,
        Payload,
        Finished,
    };

    PipeResult IssueRead() noexcept;
    PipeResult CompleteRead(DWORD transferred) noexcept;
    PipeResult Finish(PipeResult result) noexcept;
    PipeResult Deliver(Message& out) noexcept;
    void DrainOperation() noexcept;

    HANDLE m_pipe{};
    HANDLE m_operationEvent{};
    OVERLAPPED m_operation{};
    State m_state{State::Idle};
    PipeResult m_result{PipeResult::IoError};
    bool m_operationPending{};
    bool m_syncCompletion{};
    bool m_deadlineArmed{};
    DWORD m_requestedBytes{};
    DWORD m_partialFrameTimeoutMs{};
    ULONGLONG m_deadline{};
    std::array<std::uint8_t, c_headerBytes> m_wireHeader{};
    std::size_t m_headerBytesRead{};
    Header m_header{};
    std::vector<std::uint8_t> m_payload;
    std::size_t m_payloadBytesRead{};
};

// Stateful overlapped writer for one message at a time. Begin validates and
// encodes into storage owned by this object before issuing the first write.
// BeginPrepared accepts an already-framed owned buffer and transfers its
// storage into the writer after exact wire validation, avoiding a second
// full-message encoding buffer for PCM. It returns the writer's previous
// cleared buffer through the supplied message for reuse. The writer keeps the
// transferred bytes alive through completion or cancellation drain. Framing
// and validation are unchanged; the kernel still copies bytes into the pipe.
// Advance is nonblocking (apart from draining an expired/cancelled request).
// The pipe handle is borrowed and must remain valid until the operation
// completes or CancelAndDrain returns. Advance reports a completed write once.
// CancelAndDrain cancels, drains, and returns the writer to Idle; discard that
// pipe because a partial frame may have been written. A timeout is also
// terminal for its pipe. Calls on an instance must be serialized.
class AsyncPipeMessageWriter final
{
public:
    AsyncPipeMessageWriter() noexcept = default;
    ~AsyncPipeMessageWriter() noexcept;

    AsyncPipeMessageWriter(const AsyncPipeMessageWriter&) = delete;
    AsyncPipeMessageWriter& operator=(const AsyncPipeMessageWriter&) = delete;
    AsyncPipeMessageWriter(AsyncPipeMessageWriter&&) = delete;
    AsyncPipeMessageWriter& operator=(AsyncPipeMessageWriter&&) = delete;

    PipeResult Begin(HANDLE pipe, DWORD timeoutMs, const Message& message) noexcept;
    PipeResult BeginPrepared(HANDLE pipe, DWORD timeoutMs, OwnedWireMessage& message) noexcept;
    PipeResult Advance() noexcept;
    HANDLE Event() const noexcept;
    DWORD WaitTimeoutMilliseconds() const noexcept;
    void CancelAndDrain() noexcept;

private:
    enum class State : std::uint8_t
    {
        Idle,
        Writing,
        Finished,
    };

    PipeResult IssueWrite() noexcept;
    PipeResult CompleteWrite(DWORD transferred) noexcept;
    PipeResult Finish(PipeResult result) noexcept;
    PipeResult Deliver() noexcept;
    void DrainOperation() noexcept;

    HANDLE m_pipe{};
    HANDLE m_operationEvent{};
    OVERLAPPED m_operation{};
    State m_state{State::Idle};
    PipeResult m_result{PipeResult::IoError};
    bool m_operationPending{};
    bool m_syncCompletion{};
    ULONGLONG m_deadline{};
    DWORD m_requestedBytes{};
    std::size_t m_wireBytesWritten{};
    std::vector<std::uint8_t> m_wire;
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
