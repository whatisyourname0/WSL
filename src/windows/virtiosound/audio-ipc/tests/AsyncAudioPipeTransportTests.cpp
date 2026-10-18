// Copyright (C) Microsoft Corporation. All rights reserved.

#ifndef _WIN32
#error AsyncAudioPipeTransportTests require Windows.
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "AudioPipeTransport.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using wsl::virtio_sound::audio_ipc::c_headerBytes;
using wsl::virtio_sound::audio_ipc::c_maxPayloadBytes;
using wsl::virtio_sound::audio_ipc::EncodeMessage;
using wsl::virtio_sound::audio_ipc::Error;
using wsl::virtio_sound::audio_ipc::Message;
using wsl::virtio_sound::audio_ipc::MessageType;
using wsl::virtio_sound::audio_ipc::windows::AsyncPipeMessageWriter;
using wsl::virtio_sound::audio_ipc::windows::PipeMessageReader;
using wsl::virtio_sound::audio_ipc::windows::PipeResult;

int failures = 0;

bool Check(bool condition, const char* description)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
    return condition;
}

class OwnedHandle final
{
public:
    OwnedHandle() noexcept = default;
    explicit OwnedHandle(HANDLE value) noexcept : m_value(value)
    {
    }
    ~OwnedHandle()
    {
        Reset();
    }

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    HANDLE Get() const noexcept
    {
        return m_value;
    }

    explicit operator bool() const noexcept
    {
        return m_value != nullptr && m_value != INVALID_HANDLE_VALUE;
    }

    void Reset(HANDLE value = nullptr) noexcept
    {
        if (*this)
        {
            CloseHandle(m_value);
        }
        m_value = value;
    }

private:
    HANDLE m_value{};
};

Message MakeMessage(MessageType type, std::uint64_t sequence, std::vector<std::uint8_t> payload = {})
{
    Message message{};
    message.m_header.m_type = type;
    message.m_header.m_sequence = sequence;
    message.m_header.m_epoch = type == MessageType::Pcm ? 1U : 0U;
    message.m_header.m_payloadBytes = static_cast<std::uint32_t>(payload.size());
    message.m_payload = std::move(payload);
    return message;
}

bool CreatePipePair(OwnedHandle& server, OwnedHandle& client)
{
    static std::atomic<std::uint64_t> counter{};
    const std::uint64_t instance = counter.fetch_add(1U, std::memory_order_relaxed);
    const std::wstring pipeName = L"\\\\.\\pipe\\wsl-audio-ipc-async-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(instance);

    server.Reset(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1U,
        4096U,
        4096U,
        0U,
        nullptr));
    if (!server)
    {
        std::cerr << "CreateNamedPipeW failed: " << GetLastError() << '\n';
        return false;
    }

    OwnedHandle connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connectEvent)
    {
        std::cerr << "CreateEventW for pipe connection failed: " << GetLastError() << '\n';
        return false;
    }

    OVERLAPPED connectOperation{};
    connectOperation.hEvent = connectEvent.Get();
    const BOOL connectedSynchronously = ConnectNamedPipe(server.Get(), &connectOperation);
    const DWORD connectError = connectedSynchronously != FALSE ? ERROR_SUCCESS : GetLastError();
    const bool connectPending = connectError == ERROR_IO_PENDING;
    if (connectedSynchronously == FALSE && !connectPending && connectError != ERROR_PIPE_CONNECTED)
    {
        std::cerr << "ConnectNamedPipe failed: " << connectError << '\n';
        return false;
    }

    client.Reset(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING, 0U, nullptr));
    if (!client)
    {
        const DWORD openError = GetLastError();
        if (connectPending)
        {
            (void)CancelIoEx(server.Get(), &connectOperation);
            (void)WaitForSingleObject(connectEvent.Get(), INFINITE);
        }
        std::cerr << "CreateFileW for pipe client failed: " << openError << '\n';
        return false;
    }

    if (connectPending)
    {
        if (WaitForSingleObject(connectEvent.Get(), 1000U) != WAIT_OBJECT_0)
        {
            (void)CancelIoEx(server.Get(), &connectOperation);
            (void)WaitForSingleObject(connectEvent.Get(), INFINITE);
            std::cerr << "ConnectNamedPipe did not complete after client opened\n";
            return false;
        }
        DWORD ignoredBytes = 0U;
        if (GetOverlappedResult(server.Get(), &connectOperation, &ignoredBytes, FALSE) == FALSE)
        {
            std::cerr << "GetOverlappedResult for pipe connection failed: " << GetLastError() << '\n';
            return false;
        }
    }
    return true;
}

bool WriteAll(HANDLE pipe, const std::vector<std::uint8_t>& bytes)
{
    std::size_t offset = 0U;
    while (offset < bytes.size())
    {
        DWORD transferred = 0U;
        const DWORD requested = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        if (WriteFile(pipe, bytes.data() + offset, requested, &transferred, nullptr) == FALSE || transferred == 0U)
        {
            std::cerr << "WriteFile failed: " << GetLastError() << '\n';
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool ReadAll(HANDLE pipe, std::size_t byteCount, std::vector<std::uint8_t>& bytes)
{
    bytes.resize(byteCount);
    std::size_t offset = 0U;
    while (offset < byteCount)
    {
        DWORD transferred = 0U;
        const DWORD requested = static_cast<DWORD>((std::min)(byteCount - offset, static_cast<std::size_t>(MAXDWORD)));
        if (ReadFile(pipe, bytes.data() + offset, requested, &transferred, nullptr) == FALSE || transferred == 0U)
        {
            std::cerr << "ReadFile failed: " << GetLastError() << '\n';
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool Encode(const Message& message, std::vector<std::uint8_t>& wire)
{
    Error error = Error::None;
    try
    {
        if (!EncodeMessage(message, wire, error))
        {
            std::cerr << "EncodeMessage failed: " << static_cast<int>(error) << '\n';
            return false;
        }
    }
    catch (...)
    {
        std::cerr << "EncodeMessage allocation failed\n";
        return false;
    }
    return true;
}

PipeResult AwaitReader(PipeMessageReader& reader, Message& out, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        const PipeResult result = reader.Advance(out);
        if (result != PipeResult::Pending)
        {
            return result;
        }

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return reader.Advance(out);
        }
        const DWORD budget = static_cast<DWORD>(deadline - now);
        const DWORD operationWait = reader.WaitTimeoutMilliseconds();
        const DWORD waitMs = operationWait == INFINITE ? budget : (std::min)(budget, operationWait);
        const DWORD waitResult = WaitForSingleObject(reader.Event(), waitMs);
        if (waitResult == WAIT_FAILED)
        {
            return PipeResult::IoError;
        }
    }
}

PipeResult AwaitWriter(AsyncPipeMessageWriter& writer, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        const PipeResult result = writer.Advance();
        if (result != PipeResult::Pending)
        {
            return result;
        }

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return writer.Advance();
        }
        const DWORD budget = static_cast<DWORD>(deadline - now);
        const DWORD operationWait = writer.WaitTimeoutMilliseconds();
        const DWORD waitMs = operationWait == INFINITE ? budget : (std::min)(budget, operationWait);
        const DWORD waitResult = WaitForSingleObject(writer.Event(), waitMs);
        if (waitResult == WAIT_FAILED)
        {
            return PipeResult::IoError;
        }
    }
}

void TestIdleReaderPartialTimeoutAndRecovery()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for partial frame test");
    if (!server || !client)
    {
        return;
    }

    PipeMessageReader reader;
    if (!Check(reader.Begin(server.Get(), 120U) == PipeResult::Pending, "reader starts the one-byte probe"))
    {
        return;
    }
    Check(reader.WaitTimeoutMilliseconds() == INFINITE, "idle first-byte probe has no frame deadline");
    Sleep(160U);
    Message out = MakeMessage(MessageType::Hello, 100U);
    Check(reader.Advance(out) == PipeResult::Pending, "idle reader stays pending beyond the partial-frame timeout");
    Check(reader.WaitTimeoutMilliseconds() == INFINITE, "idle reader still reports an infinite wait");

    const Message input = MakeMessage(MessageType::Pcm, 11U, std::vector<std::uint8_t>(64U, 0x31U));
    std::vector<std::uint8_t> wire;
    if (!Check(Encode(input, wire), "PCM frame encodes for partial read"))
    {
        return;
    }
    const std::size_t partialHeaderBytes = 5U;
    std::vector<std::uint8_t> prefix(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(partialHeaderBytes));
    if (!Check(WriteAll(client.Get(), prefix), "partial header reaches the reader") ||
        !Check(WaitForSingleObject(reader.Event(), 1000U) == WAIT_OBJECT_0, "first frame byte completes the probe") ||
        !Check(reader.Advance(out) == PipeResult::Pending, "partial frame remains pending"))
    {
        reader.CancelAndDrain();
        return;
    }
    Check(reader.WaitTimeoutMilliseconds() <= 120U, "partial frame has a bounded remaining deadline");
    Sleep(180U);
    if (!Check(reader.Advance(out) == PipeResult::Timeout, "partial frame expires and drains its outstanding read"))
    {
        reader.CancelAndDrain();
        return;
    }

    OwnedHandle secondServer;
    OwnedHandle secondClient;
    Check(CreatePipePair(secondServer, secondClient), "fresh pipe pair opens after timeout");
    if (!secondServer || !secondClient)
    {
        return;
    }
    if (!Check(reader.Begin(secondServer.Get(), 1000U) == PipeResult::Pending, "reader reuses its event and storage after timeout"))
    {
        return;
    }
    const std::size_t partialPayloadBytes = c_headerBytes + 13U;
    std::vector<std::uint8_t> payloadPrefix(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(partialPayloadBytes));
    std::vector<std::uint8_t> payloadSuffix(wire.begin() + static_cast<std::ptrdiff_t>(partialPayloadBytes), wire.end());
    if (!Check(WriteAll(secondClient.Get(), payloadPrefix), "partial payload reaches reader after timeout recovery") ||
        !Check(AwaitReader(reader, out, 100U) == PipeResult::Pending, "reader waits for the rest of a partial payload") ||
        !Check(WriteAll(secondClient.Get(), payloadSuffix), "remaining payload reaches the reader"))
    {
        reader.CancelAndDrain();
        return;
    }
    if (!Check(AwaitReader(reader, out, 1000U) == PipeResult::Success, "reader completes the recovered frame"))
    {
        reader.CancelAndDrain();
        return;
    }
    Check(out.m_header.m_sequence == input.m_header.m_sequence && out.m_payload == input.m_payload, "recovered frame preserves exact payload bytes");
}

void TestInvalidHeaderBeforePayloadAndOutPreservation()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for invalid header test");
    if (!server || !client)
    {
        return;
    }

    Message oversized = MakeMessage(MessageType::Pcm, 12U, std::vector<std::uint8_t>(4U, 0x11U));
    std::vector<std::uint8_t> invalidWire;
    if (!Check(Encode(oversized, invalidWire), "valid template frame encodes"))
    {
        return;
    }
    invalidWire[8U] = static_cast<std::uint8_t>((c_maxPayloadBytes + 1U) & 0xFFU);
    invalidWire[9U] = static_cast<std::uint8_t>(((c_maxPayloadBytes + 1U) >> 8U) & 0xFFU);
    invalidWire[10U] = static_cast<std::uint8_t>(((c_maxPayloadBytes + 1U) >> 16U) & 0xFFU);
    invalidWire[11U] = static_cast<std::uint8_t>(((c_maxPayloadBytes + 1U) >> 24U) & 0xFFU);
    invalidWire.resize(c_headerBytes);

    PipeMessageReader reader;
    Message out = MakeMessage(MessageType::Hello, 99U);
    if (!Check(reader.Begin(server.Get(), 1000U) == PipeResult::Pending, "reader starts before invalid header arrives") ||
        !Check(WriteAll(client.Get(), invalidWire), "oversized header reaches reader without a payload"))
    {
        reader.CancelAndDrain();
        return;
    }
    if (!Check(AwaitReader(reader, out, 1000U) == PipeResult::InvalidMessage, "invalid payload length is rejected from the header alone"))
    {
        reader.CancelAndDrain();
        return;
    }
    Check(out.m_header.m_sequence == 99U, "failed read leaves the caller message unchanged");
}

void TestConcurrentReadWriteAndBufferReuse()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "duplex pipe pair opens for concurrent read and write");
    if (!server || !client)
    {
        return;
    }

    PipeMessageReader reader;
    AsyncPipeMessageWriter writer;
    Message received{};
    const Message inbound = MakeMessage(MessageType::Hello, 21U);
    const Message outbound = MakeMessage(MessageType::Hello, 22U);
    std::vector<std::uint8_t> inboundWire;
    std::vector<std::uint8_t> outboundWire;
    if (!Check(Encode(inbound, inboundWire), "inbound Hello encodes") ||
        !Check(Encode(outbound, outboundWire), "outbound Hello encodes"))
    {
        return;
    }

    if (!Check(reader.Begin(server.Get(), 1000U) == PipeResult::Pending, "duplex reader begins"))
    {
        return;
    }
    if (!Check(writer.Begin(server.Get(), 1000U, outbound) == PipeResult::Pending, "duplex writer begins concurrently"))
    {
        reader.CancelAndDrain();
        return;
    }
    if (!Check(WriteAll(client.Get(), inboundWire), "client sends concurrent inbound frame"))
    {
        reader.CancelAndDrain();
        writer.CancelAndDrain();
        return;
    }
    std::vector<std::uint8_t> clientReceived;
    if (!Check(ReadAll(client.Get(), outboundWire.size(), clientReceived), "client receives concurrent outbound frame") ||
        !Check(clientReceived == outboundWire, "async writer emits exact wire bytes"))
    {
        reader.CancelAndDrain();
        writer.CancelAndDrain();
        return;
    }
    if (!Check(AwaitReader(reader, received, 1000U) == PipeResult::Success, "async reader collects concurrent inbound frame"))
    {
        reader.CancelAndDrain();
        writer.CancelAndDrain();
        return;
    }
    if (!Check(AwaitWriter(writer, 1000U) == PipeResult::Success, "async writer collects completion"))
    {
        writer.CancelAndDrain();
        return;
    }
    Check(received.m_header.m_sequence == inbound.m_header.m_sequence, "async reader decodes the inbound sequence");

    const Message secondOutbound = MakeMessage(MessageType::Hello, 23U);
    std::vector<std::uint8_t> secondWire;
    if (!Check(Encode(secondOutbound, secondWire), "second Hello encodes") ||
        !Check(writer.Begin(server.Get(), 1000U, secondOutbound) == PipeResult::Pending, "writer begins a second frame using retained storage"))
    {
        return;
    }
    if (!Check(ReadAll(client.Get(), secondWire.size(), clientReceived), "client receives second async frame") ||
        !Check(clientReceived == secondWire, "reused writer buffer contains only the second frame") ||
        !Check(AwaitWriter(writer, 1000U) == PipeResult::Success, "reused writer completes second frame"))
    {
        writer.CancelAndDrain();
    }
}

void TestCancellationAndWriterTimeoutRecovery()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for cancellation test");
    if (!server || !client)
    {
        return;
    }

    PipeMessageReader reader;
    Message out{};
    if (!Check(reader.Begin(server.Get(), 1000U) == PipeResult::Pending, "reader starts before cancellation"))
    {
        return;
    }
    Check(reader.Begin(server.Get(), 1000U) == PipeResult::IoError, "reader rejects a second Begin while the first read is pending");
    reader.CancelAndDrain();
    Check(reader.Advance(out) == PipeResult::IoError, "drained reader returns to idle after explicit cancellation");

    server.Reset();
    client.Reset();
    OwnedHandle reuseServer;
    OwnedHandle reuseClient;
    Check(CreatePipePair(reuseServer, reuseClient), "fresh pipe pair opens after reader cancellation");
    if (!reuseServer || !reuseClient)
    {
        return;
    }
    if (!Check(reader.Begin(reuseServer.Get(), 1000U) == PipeResult::Pending, "reader reuses its event after cancellation"))
    {
        return;
    }
    const Message smallInbound = MakeMessage(MessageType::Hello, 33U);
    std::vector<std::uint8_t> smallInboundWire;
    if (!Check(Encode(smallInbound, smallInboundWire), "post-cancellation Hello encodes") ||
        !Check(WriteAll(reuseClient.Get(), smallInboundWire), "post-cancellation frame reaches the reader"))
    {
        reader.CancelAndDrain();
        return;
    }
    if (!Check(AwaitReader(reader, out, 1000U) == PipeResult::Success, "reader completes a frame on a fresh pipe after cancellation"))
    {
        reader.CancelAndDrain();
        return;
    }

    AsyncPipeMessageWriter writer;
    Message largeMessage = MakeMessage(MessageType::Pcm, 9U, std::vector<std::uint8_t>(c_maxPayloadBytes, 0x2AU));
    if (!Check(writer.Begin(reuseServer.Get(), 10000U, largeMessage) == PipeResult::Pending, "large writer starts against an unread pipe"))
    {
        return;
    }
    Check(writer.Begin(reuseServer.Get(), 10000U, MakeMessage(MessageType::Hello, 10U)) == PipeResult::IoError, "writer rejects a second Begin while the first write is pending");
    writer.CancelAndDrain();
    Check(writer.Advance() == PipeResult::IoError, "drained writer returns to idle after explicit cancellation");

    reuseServer.Reset();
    reuseClient.Reset();
    OwnedHandle timeoutServer;
    OwnedHandle timeoutClient;
    Check(CreatePipePair(timeoutServer, timeoutClient), "fresh pipe pair opens for async writer timeout");
    if (!timeoutServer || !timeoutClient)
    {
        return;
    }
    if (!Check(writer.Begin(timeoutServer.Get(), 75U, largeMessage) == PipeResult::Pending, "large writer starts for timeout test"))
    {
        return;
    }
    Check(writer.WaitTimeoutMilliseconds() <= 75U, "writer exposes its remaining deadline");
    if (!Check(AwaitWriter(writer, 500U) == PipeResult::Timeout, "large async write times out and drains"))
    {
        writer.CancelAndDrain();
        return;
    }

    timeoutServer.Reset();
    timeoutClient.Reset();
    OwnedHandle freshServer;
    OwnedHandle freshClient;
    Check(CreatePipePair(freshServer, freshClient), "fresh pipe pair opens after async write timeout");
    if (!freshServer || !freshClient)
    {
        return;
    }
    const Message smallMessage = MakeMessage(MessageType::Hello, 44U);
    std::vector<std::uint8_t> smallWire;
    if (!Check(Encode(smallMessage, smallWire), "recovery Hello encodes") ||
        !Check(writer.Begin(freshServer.Get(), 1000U, smallMessage) == PipeResult::Pending, "writer safely reuses its wire buffer after timeout"))
    {
        return;
    }
    std::vector<std::uint8_t> received;
    if (!Check(ReadAll(freshClient.Get(), smallWire.size(), received), "fresh client receives reused writer buffer") ||
        !Check(received == smallWire, "reused writer sends the exact new frame") ||
        !Check(AwaitWriter(writer, 1000U) == PipeResult::Success, "writer completion is collected after reuse"))
    {
        writer.CancelAndDrain();
    }
}

} // namespace

int main()
{
    TestIdleReaderPartialTimeoutAndRecovery();
    TestInvalidHeaderBeforePayloadAndOutPreservation();
    TestConcurrentReadWriteAndBufferReuse();
    TestCancellationAndWriterTimeoutRecovery();

    if (failures != 0)
    {
        std::cerr << failures << " async transport test(s) failed\n";
        return 1;
    }
    std::cout << "Async audio IPC transport tests passed\n";
    return 0;
}
