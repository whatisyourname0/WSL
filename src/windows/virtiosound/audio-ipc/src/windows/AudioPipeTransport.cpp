// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "AudioPipeTransport.hpp"

namespace wsl::virtio_sound::audio_ipc::windows {
namespace {

    class Deadline final
    {
    public:
        explicit Deadline(DWORD timeoutMs) noexcept : m_endTick(GetTickCount64() + static_cast<std::uint64_t>(timeoutMs))
        {
        }

        bool Remaining(DWORD& remainingMs) const noexcept
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= m_endTick)
            {
                remainingMs = 0U;
                return false;
            }
            const std::uint64_t remaining = m_endTick - now;
            remainingMs = static_cast<DWORD>((std::min)(remaining, static_cast<std::uint64_t>(MAXDWORD)));
            return true;
        }

    private:
        std::uint64_t m_endTick;
    };

    bool IsDisconnectedError(DWORD error) noexcept
    {
        return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA || error == ERROR_INVALID_HANDLE;
    }

    PipeResult ResultFromError(DWORD error) noexcept
    {
        return IsDisconnectedError(error) ? PipeResult::Disconnected : PipeResult::IoError;
    }

    void CancelAndDrain(HANDLE pipe, OVERLAPPED& operation) noexcept
    {
        (void)CancelIoEx(pipe, &operation);
        // The OVERLAPPED and its event must remain alive until the kernel reports
        // completion, including ERROR_OPERATION_ABORTED, after cancellation. This
        // drain is required for memory safety and has no hard wall-clock bound if
        // the OS stalls completion.
        const DWORD waitResult = WaitForSingleObject(operation.hEvent, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
        {
            DWORD ignoredBytes = 0U;
            (void)GetOverlappedResult(pipe, &operation, &ignoredBytes, FALSE);
        }
    }

    PipeResult TransferOne(
        HANDLE pipe,
        HANDLE stopEvent,
        const Deadline& deadline,
        HANDLE operationEvent,
        bool writing,
        std::uint8_t* readDestination,
        const std::uint8_t* writeSource,
        DWORD requestedBytes,
        DWORD& transferredBytes) noexcept
    {
        transferredBytes = 0U;
        if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || requestedBytes == 0U ||
            (writing ? writeSource == nullptr : readDestination == nullptr))
        {
            return PipeResult::IoError;
        }
        if (stopEvent != nullptr)
        {
            const DWORD stopState = WaitForSingleObject(stopEvent, 0U);
            if (stopState == WAIT_OBJECT_0)
            {
                return PipeResult::Cancelled;
            }
            if (stopState == WAIT_FAILED)
            {
                return PipeResult::IoError;
            }
        }

        DWORD remainingMs = 0U;
        if (!deadline.Remaining(remainingMs))
        {
            return PipeResult::Timeout;
        }

        if (!ResetEvent(operationEvent))
        {
            return PipeResult::IoError;
        }
        OVERLAPPED operation{};
        operation.hEvent = operationEvent;

        const BOOL completedSynchronously = writing ? WriteFile(pipe, writeSource, requestedBytes, nullptr, &operation)
                                                    : ReadFile(pipe, readDestination, requestedBytes, nullptr, &operation);
        if (completedSynchronously != FALSE)
        {
            if (GetOverlappedResult(pipe, &operation, &transferredBytes, FALSE) == FALSE)
            {
                return ResultFromError(GetLastError());
            }
            if (transferredBytes == 0U)
            {
                return PipeResult::Disconnected;
            }
            return PipeResult::Success;
        }

        const DWORD startError = GetLastError();
        if (startError != ERROR_IO_PENDING)
        {
            return ResultFromError(startError);
        }

        HANDLE waitHandles[2] = {operationEvent, stopEvent};
        const DWORD handleCount = stopEvent == nullptr ? 1U : 2U;
        remainingMs = 0U;
        if (!deadline.Remaining(remainingMs))
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::Timeout;
        }

        const DWORD waitResult = WaitForMultipleObjects(handleCount, waitHandles, FALSE, remainingMs);
        if (waitResult == WAIT_OBJECT_0 + 1U && handleCount == 2U)
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::Cancelled;
        }
        if (waitResult == WAIT_TIMEOUT)
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::Timeout;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            CancelAndDrain(pipe, operation);
            return PipeResult::IoError;
        }

        if (GetOverlappedResult(pipe, &operation, &transferredBytes, FALSE) == FALSE)
        {
            const DWORD completionError = GetLastError();
            if (completionError == ERROR_MORE_DATA && !writing && transferredBytes != 0U)
            {
                return PipeResult::Success;
            }
            if (completionError == ERROR_OPERATION_ABORTED && stopEvent != nullptr && WaitForSingleObject(stopEvent, 0U) == WAIT_OBJECT_0)
            {
                return PipeResult::Cancelled;
            }
            return ResultFromError(completionError);
        }
        if (transferredBytes == 0U)
        {
            return PipeResult::Disconnected;
        }
        return PipeResult::Success;
    }

    PipeResult ReadExact(HANDLE pipe, HANDLE stopEvent, const Deadline& deadline, HANDLE operationEvent, std::uint8_t* destination, std::size_t byteCount) noexcept
    {
        std::size_t offset = 0U;
        while (offset < byteCount)
        {
            const std::size_t remaining = byteCount - offset;
            const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD transferred = 0U;
            const PipeResult result =
                TransferOne(pipe, stopEvent, deadline, operationEvent, false, destination + offset, nullptr, chunk, transferred);
            if (result != PipeResult::Success)
            {
                return result;
            }
            if (transferred > chunk)
            {
                return PipeResult::IoError;
            }
            offset += transferred;
        }
        return PipeResult::Success;
    }

    PipeResult WriteExact(HANDLE pipe, HANDLE stopEvent, const Deadline& deadline, HANDLE operationEvent, const std::uint8_t* source, std::size_t byteCount) noexcept
    {
        std::size_t offset = 0U;
        while (offset < byteCount)
        {
            const std::size_t remaining = byteCount - offset;
            const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD transferred = 0U;
            const PipeResult result =
                TransferOne(pipe, stopEvent, deadline, operationEvent, true, nullptr, source + offset, chunk, transferred);
            if (result != PipeResult::Success)
            {
                return result;
            }
            if (transferred > chunk)
            {
                return PipeResult::IoError;
            }
            offset += transferred;
        }
        return PipeResult::Success;
    }

} // namespace

PipeResult ReadMessage(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, Message& out) noexcept
{
    if (timeoutMs == INFINITE || pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
    {
        return PipeResult::IoError;
    }

    wil::unique_event event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event)
    {
        return PipeResult::IoError;
    }
    const Deadline deadline(timeoutMs);

    std::array<std::uint8_t, c_headerBytes> wireHeader{};
    PipeResult result = ReadExact(pipe, stopEvent, deadline, event.get(), wireHeader.data(), wireHeader.size());
    if (result != PipeResult::Success)
    {
        return result;
    }

    Header header{};
    Error codecError = Error::None;
    if (!DecodeHeader(wireHeader.data(), wireHeader.size(), header, codecError))
    {
        return PipeResult::InvalidMessage;
    }

    Message decoded{};
    decoded.m_header = header;
    try
    {
        decoded.m_payload.resize(header.m_payloadBytes);
    }
    catch (...)
    {
        return PipeResult::IoError;
    }

    if (!decoded.m_payload.empty())
    {
        result = ReadExact(pipe, stopEvent, deadline, event.get(), decoded.m_payload.data(), decoded.m_payload.size());
        if (result != PipeResult::Success)
        {
            return result;
        }
    }

    if (!ValidateMessage(decoded, codecError))
    {
        return PipeResult::InvalidMessage;
    }
    out = std::move(decoded);
    return PipeResult::Success;
}

PipeMessageWriter::~PipeMessageWriter() noexcept
{
    if (m_operationEvent != nullptr)
    {
        CloseHandle(m_operationEvent);
    }
}

PipeResult PipeMessageWriter::Write(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, const Message& message) noexcept
{
    if (timeoutMs == INFINITE || pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
    {
        return PipeResult::IoError;
    }

    Error codecError = Error::None;
    if (!ValidateMessage(message, codecError))
    {
        return PipeResult::InvalidMessage;
    }

    try
    {
        if (!EncodeMessage(message, m_wire, codecError))
        {
            return PipeResult::InvalidMessage;
        }
    }
    catch (...)
    {
        return PipeResult::IoError;
    }

    if (m_operationEvent == nullptr)
    {
        m_operationEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_operationEvent == nullptr)
        {
            return PipeResult::IoError;
        }
    }

    const Deadline deadline(timeoutMs);
    return WriteExact(pipe, stopEvent, deadline, m_operationEvent, m_wire.data(), m_wire.size());
}

PipeResult WriteMessage(HANDLE pipe, HANDLE stopEvent, DWORD timeoutMs, const Message& message) noexcept
{
    PipeMessageWriter writer;
    return writer.Write(pipe, stopEvent, timeoutMs, message);
}

namespace {

    enum class CompletionPoll
    {
        Pending,
        Completed,
        Failed,
        WaitFailed,
    };

    CompletionPoll PollOverlapped(
        HANDLE pipe,
        HANDLE event,
        OVERLAPPED& operation,
        bool& operationPending,
        bool& syncCompletion,
        DWORD& transferred,
        PipeResult& failure) noexcept
    {
        transferred = 0U;
        if (!syncCompletion)
        {
            const DWORD waitResult = WaitForSingleObject(event, 0U);
            if (waitResult == WAIT_TIMEOUT)
            {
                return CompletionPoll::Pending;
            }
            if (waitResult != WAIT_OBJECT_0)
            {
                return CompletionPoll::WaitFailed;
            }
        }

        if (GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE)
        {
            operationPending = false;
            syncCompletion = false;
            return CompletionPoll::Completed;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE)
        {
            syncCompletion = false;
            return CompletionPoll::Pending;
        }
        if (error == ERROR_MORE_DATA && transferred != 0U)
        {
            operationPending = false;
            syncCompletion = false;
            return CompletionPoll::Completed;
        }

        operationPending = false;
        syncCompletion = false;
        failure = error == ERROR_OPERATION_ABORTED ? PipeResult::Cancelled : ResultFromError(error);
        return CompletionPoll::Failed;
    }

    bool EnsureManualResetEvent(HANDLE& event) noexcept
    {
        if (event != nullptr)
        {
            return true;
        }
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return event != nullptr;
    }

    bool DeadlineExpired(ULONGLONG deadline) noexcept
    {
        return GetTickCount64() >= deadline;
    }

    DWORD DeadlineRemaining(ULONGLONG deadline) noexcept
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return 0U;
        }
        const ULONGLONG remaining = deadline - now;
        return static_cast<DWORD>((std::min)(remaining, static_cast<ULONGLONG>(MAXDWORD)));
    }

    void CancelAndDrainOverlapped(HANDLE pipe, HANDLE event, OVERLAPPED& operation, bool& operationPending, bool& syncCompletion) noexcept
    {
        if (!operationPending)
        {
            return;
        }

        (void)CancelIoEx(pipe, &operation);
        // Keep both OVERLAPPED and the buffers it refers to alive until the OS
        // signals completion. If the event wait itself fails, ask the kernel to
        // wait for the operation directly before allowing storage to be reused.
        const DWORD waitResult = WaitForSingleObject(event, INFINITE);
        DWORD ignoredBytes = 0U;
        if (waitResult == WAIT_OBJECT_0)
        {
            if (GetOverlappedResult(pipe, &operation, &ignoredBytes, FALSE) == FALSE && GetLastError() == ERROR_IO_INCOMPLETE)
            {
                (void)GetOverlappedResult(pipe, &operation, &ignoredBytes, TRUE);
            }
        }
        else
        {
            (void)GetOverlappedResult(pipe, &operation, &ignoredBytes, TRUE);
        }
        operationPending = false;
        syncCompletion = false;
    }

} // namespace

PipeMessageReader::~PipeMessageReader() noexcept
{
    CancelAndDrain();
    if (m_operationEvent != nullptr)
    {
        CloseHandle(m_operationEvent);
        m_operationEvent = nullptr;
    }
}

PipeResult PipeMessageReader::Begin(HANDLE pipe, DWORD partialFrameTimeoutMs) noexcept
{
    if (m_state != State::Idle || pipe == nullptr || pipe == INVALID_HANDLE_VALUE || partialFrameTimeoutMs == INFINITE)
    {
        return PipeResult::IoError;
    }
    if (!EnsureManualResetEvent(m_operationEvent) || ResetEvent(m_operationEvent) == FALSE)
    {
        return PipeResult::IoError;
    }

    m_pipe = pipe;
    m_partialFrameTimeoutMs = partialFrameTimeoutMs;
    m_deadlineArmed = false;
    m_headerBytesRead = 0U;
    m_payloadBytesRead = 0U;
    m_payload.clear();
    m_wireHeader.fill(0U);
    m_state = State::FirstByte;

    const PipeResult result = IssueRead();
    if (result != PipeResult::Pending)
    {
        m_state = State::Idle;
        m_pipe = nullptr;
    }
    return result;
}

PipeResult PipeMessageReader::IssueRead() noexcept
{
    std::uint8_t* destination = nullptr;
    std::size_t requested = 0U;
    switch (m_state)
    {
    case State::FirstByte:
        destination = m_wireHeader.data();
        requested = 1U;
        break;
    case State::Header:
        destination = m_wireHeader.data() + m_headerBytesRead;
        requested = c_headerBytes - m_headerBytesRead;
        break;
    case State::Payload:
        if (m_payloadBytesRead >= m_payload.size())
        {
            return PipeResult::IoError;
        }
        destination = m_payload.data() + m_payloadBytesRead;
        requested = m_payload.size() - m_payloadBytesRead;
        break;
    default:
        return PipeResult::IoError;
    }
    if (destination == nullptr || requested == 0U || requested > static_cast<std::size_t>(MAXDWORD))
    {
        return PipeResult::IoError;
    }

    if (ResetEvent(m_operationEvent) == FALSE)
    {
        return PipeResult::IoError;
    }
    m_operation = {};
    m_operation.hEvent = m_operationEvent;
    m_requestedBytes = static_cast<DWORD>(requested);

    if (ReadFile(m_pipe, destination, m_requestedBytes, nullptr, &m_operation) != FALSE)
    {
        m_operationPending = true;
        m_syncCompletion = true;
        return PipeResult::Pending;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
    {
        m_operationPending = true;
        m_syncCompletion = false;
        return PipeResult::Pending;
    }
    if (error == ERROR_MORE_DATA)
    {
        // ERROR_MORE_DATA means the request completed with bytes in a message-
        // mode pipe. Collect its count through GetOverlappedResult like any
        // other completion and continue the exact-length read.
        m_operationPending = true;
        m_syncCompletion = true;
        return PipeResult::Pending;
    }
    return ResultFromError(error);
}

PipeResult PipeMessageReader::CompleteRead(DWORD transferred) noexcept
{
    if (transferred == 0U)
    {
        return Finish(PipeResult::Disconnected);
    }
    if (transferred > m_requestedBytes)
    {
        return Finish(PipeResult::IoError);
    }

    if (m_state == State::FirstByte)
    {
        if (transferred != 1U)
        {
            return Finish(PipeResult::IoError);
        }
        m_headerBytesRead = 1U;
        m_deadline = GetTickCount64() + static_cast<ULONGLONG>(m_partialFrameTimeoutMs);
        m_deadlineArmed = true;
        m_state = State::Header;
    }
    else if (m_state == State::Header)
    {
        m_headerBytesRead += transferred;
        if (m_headerBytesRead == c_headerBytes)
        {
            Error codecError = Error::None;
            Header decodedHeader{};
            if (!DecodeHeader(m_wireHeader.data(), m_wireHeader.size(), decodedHeader, codecError))
            {
                return Finish(PipeResult::InvalidMessage);
            }
            m_header = decodedHeader;
            try
            {
                m_payload.resize(m_header.m_payloadBytes);
            }
            catch (...)
            {
                return Finish(PipeResult::IoError);
            }
            if (m_payload.empty())
            {
                Message completed{};
                completed.m_header = m_header;
                Error validationError = Error::None;
                if (!ValidateMessage(completed, validationError))
                {
                    return Finish(PipeResult::InvalidMessage);
                }
                return Finish(PipeResult::Success);
            }
            m_state = State::Payload;
        }
    }
    else if (m_state == State::Payload)
    {
        m_payloadBytesRead += transferred;
        if (m_payloadBytesRead == m_payload.size())
        {
            Message completed{};
            completed.m_header = m_header;
            completed.m_payload = std::move(m_payload);
            Error validationError = Error::None;
            if (!ValidateMessage(completed, validationError))
            {
                return Finish(PipeResult::InvalidMessage);
            }
            m_payload = std::move(completed.m_payload);
            return Finish(PipeResult::Success);
        }
    }
    else
    {
        return Finish(PipeResult::IoError);
    }

    const PipeResult result = IssueRead();
    return result == PipeResult::Pending ? result : Finish(result);
}

PipeResult PipeMessageReader::Finish(PipeResult result) noexcept
{
    m_result = result;
    m_state = State::Finished;
    if (result != PipeResult::Success)
    {
        m_payload.clear();
    }
    return result;
}

PipeResult PipeMessageReader::Deliver(Message& out) noexcept
{
    if (m_state != State::Finished)
    {
        return PipeResult::Pending;
    }
    const PipeResult result = m_result;
    if (result == PipeResult::Success)
    {
        out.m_header = m_header;
        out.m_payload = std::move(m_payload);
    }
    m_state = State::Idle;
    m_result = PipeResult::IoError;
    m_pipe = nullptr;
    m_deadlineArmed = false;
    m_operationPending = false;
    m_syncCompletion = false;
    m_headerBytesRead = 0U;
    m_payloadBytesRead = 0U;
    m_requestedBytes = 0U;
    return result;
}

PipeResult PipeMessageReader::Advance(Message& out) noexcept
{
    for (;;)
    {
        if (m_state == State::Finished)
        {
            return Deliver(out);
        }
        if (m_state == State::Idle || !m_operationPending)
        {
            return PipeResult::IoError;
        }

        DWORD transferred = 0U;
        PipeResult failure = PipeResult::IoError;
        const CompletionPoll poll = PollOverlapped(
            m_pipe,
            m_operationEvent,
            m_operation,
            m_operationPending,
            m_syncCompletion,
            transferred,
            failure);
        if (poll == CompletionPoll::Pending)
        {
            if (m_deadlineArmed && DeadlineExpired(m_deadline))
            {
                DrainOperation();
                Finish(PipeResult::Timeout);
                return Deliver(out);
            }
            return PipeResult::Pending;
        }
        if (poll == CompletionPoll::WaitFailed)
        {
            DrainOperation();
            Finish(PipeResult::IoError);
            return Deliver(out);
        }
        if (poll == CompletionPoll::Failed)
        {
            Finish(failure);
            return Deliver(out);
        }

        (void)CompleteRead(transferred);
        if (m_state == State::Finished)
        {
            return Deliver(out);
        }
        if (!m_operationPending)
        {
            Finish(PipeResult::IoError);
            return Deliver(out);
        }
        if (!m_syncCompletion)
        {
            return PipeResult::Pending;
        }
        // Process immediately completed follow-up reads in this Advance call;
        // callers do not need to rely on a separately signaled event for them.
    }
}

HANDLE PipeMessageReader::Event() const noexcept
{
    return m_operationEvent;
}

DWORD PipeMessageReader::WaitTimeoutMilliseconds() const noexcept
{
    if (m_state == State::Idle || !m_deadlineArmed)
    {
        return INFINITE;
    }
    return DeadlineRemaining(m_deadline);
}

void PipeMessageReader::DrainOperation() noexcept
{
    CancelAndDrainOverlapped(m_pipe, m_operationEvent, m_operation, m_operationPending, m_syncCompletion);
}

void PipeMessageReader::CancelAndDrain() noexcept
{
    if (m_state == State::Idle || m_state == State::Finished)
    {
        return;
    }
    DrainOperation();
    m_state = State::Idle;
    m_result = PipeResult::IoError;
    m_pipe = nullptr;
    m_deadlineArmed = false;
    m_headerBytesRead = 0U;
    m_payloadBytesRead = 0U;
    m_requestedBytes = 0U;
    m_payload.clear();
}

AsyncPipeMessageWriter::~AsyncPipeMessageWriter() noexcept
{
    CancelAndDrain();
    if (m_operationEvent != nullptr)
    {
        CloseHandle(m_operationEvent);
        m_operationEvent = nullptr;
    }
}

PipeResult AsyncPipeMessageWriter::Begin(HANDLE pipe, DWORD timeoutMs, const Message& message) noexcept
{
    if (m_state != State::Idle || pipe == nullptr || pipe == INVALID_HANDLE_VALUE || timeoutMs == INFINITE)
    {
        return PipeResult::IoError;
    }

    Error codecError = Error::None;
    if (!ValidateMessage(message, codecError))
    {
        return PipeResult::InvalidMessage;
    }
    try
    {
        if (!EncodeMessage(message, m_wire, codecError))
        {
            return PipeResult::InvalidMessage;
        }
    }
    catch (...)
    {
        return PipeResult::IoError;
    }
    if (m_wire.empty() || !EnsureManualResetEvent(m_operationEvent) || ResetEvent(m_operationEvent) == FALSE)
    {
        return PipeResult::IoError;
    }

    m_pipe = pipe;
    m_deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    m_wireBytesWritten = 0U;
    m_state = State::Writing;

    const PipeResult result = IssueWrite();
    if (result != PipeResult::Pending)
    {
        m_state = State::Idle;
        m_pipe = nullptr;
    }
    return result;
}

PipeResult AsyncPipeMessageWriter::IssueWrite() noexcept
{
    if (m_state != State::Writing || m_wireBytesWritten >= m_wire.size())
    {
        return PipeResult::IoError;
    }
    const std::size_t remaining = m_wire.size() - m_wireBytesWritten;
    if (remaining > static_cast<std::size_t>(MAXDWORD))
    {
        return PipeResult::IoError;
    }
    if (ResetEvent(m_operationEvent) == FALSE)
    {
        return PipeResult::IoError;
    }
    m_operation = {};
    m_operation.hEvent = m_operationEvent;
    m_requestedBytes = static_cast<DWORD>(remaining);

    if (WriteFile(m_pipe, m_wire.data() + m_wireBytesWritten, m_requestedBytes, nullptr, &m_operation) != FALSE)
    {
        m_operationPending = true;
        m_syncCompletion = true;
        return PipeResult::Pending;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING || error == ERROR_MORE_DATA)
    {
        m_operationPending = true;
        m_syncCompletion = error == ERROR_MORE_DATA;
        return PipeResult::Pending;
    }
    return ResultFromError(error);
}

PipeResult AsyncPipeMessageWriter::CompleteWrite(DWORD transferred) noexcept
{
    if (transferred == 0U)
    {
        return Finish(PipeResult::Disconnected);
    }
    if (transferred > m_requestedBytes || transferred > m_wire.size() - m_wireBytesWritten)
    {
        return Finish(PipeResult::IoError);
    }
    m_wireBytesWritten += transferred;
    if (m_wireBytesWritten == m_wire.size())
    {
        return Finish(PipeResult::Success);
    }

    const PipeResult result = IssueWrite();
    return result == PipeResult::Pending ? result : Finish(result);
}

PipeResult AsyncPipeMessageWriter::Finish(PipeResult result) noexcept
{
    m_result = result;
    m_state = State::Finished;
    return result;
}

PipeResult AsyncPipeMessageWriter::Deliver() noexcept
{
    if (m_state != State::Finished)
    {
        return PipeResult::Pending;
    }
    const PipeResult result = m_result;
    m_state = State::Idle;
    m_result = PipeResult::IoError;
    m_pipe = nullptr;
    m_operationPending = false;
    m_syncCompletion = false;
    m_requestedBytes = 0U;
    m_wireBytesWritten = 0U;
    return result;
}

PipeResult AsyncPipeMessageWriter::Advance() noexcept
{
    for (;;)
    {
        if (m_state == State::Finished)
        {
            return Deliver();
        }
        if (m_state == State::Idle || !m_operationPending)
        {
            return PipeResult::IoError;
        }

        DWORD transferred = 0U;
        PipeResult failure = PipeResult::IoError;
        const CompletionPoll poll = PollOverlapped(
            m_pipe,
            m_operationEvent,
            m_operation,
            m_operationPending,
            m_syncCompletion,
            transferred,
            failure);
        if (poll == CompletionPoll::Pending)
        {
            if (DeadlineExpired(m_deadline))
            {
                DrainOperation();
                Finish(PipeResult::Timeout);
                return Deliver();
            }
            return PipeResult::Pending;
        }
        if (poll == CompletionPoll::WaitFailed)
        {
            DrainOperation();
            Finish(PipeResult::IoError);
            return Deliver();
        }
        if (poll == CompletionPoll::Failed)
        {
            Finish(failure);
            return Deliver();
        }

        (void)CompleteWrite(transferred);
        if (m_state == State::Finished)
        {
            return Deliver();
        }
        if (!m_operationPending)
        {
            Finish(PipeResult::IoError);
            return Deliver();
        }
        if (!m_syncCompletion)
        {
            return PipeResult::Pending;
        }
        // A synchronous partial write may immediately issue the next chunk;
        // consume it here without requiring an event wait.
    }
}

HANDLE AsyncPipeMessageWriter::Event() const noexcept
{
    return m_operationEvent;
}

DWORD AsyncPipeMessageWriter::WaitTimeoutMilliseconds() const noexcept
{
    if (m_state == State::Idle)
    {
        return INFINITE;
    }
    return DeadlineRemaining(m_deadline);
}

void AsyncPipeMessageWriter::DrainOperation() noexcept
{
    CancelAndDrainOverlapped(m_pipe, m_operationEvent, m_operation, m_operationPending, m_syncCompletion);
}

void AsyncPipeMessageWriter::CancelAndDrain() noexcept
{
    if (m_state == State::Idle || m_state == State::Finished)
    {
        return;
    }
    DrainOperation();
    m_state = State::Idle;
    m_result = PipeResult::IoError;
    m_pipe = nullptr;
    m_requestedBytes = 0U;
    m_wireBytesWritten = 0U;
}

} // namespace wsl::virtio_sound::audio_ipc::windows
