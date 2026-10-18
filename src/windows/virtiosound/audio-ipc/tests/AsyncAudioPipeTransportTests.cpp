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
#include <array>
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
using wsl::virtio_sound::audio_ipc::OwnedWireMessage;
using wsl::virtio_sound::audio_ipc::PreparePcmMessage;
using wsl::virtio_sound::audio_ipc::ValidateWireMessage;
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

bool PreparePcmWire(OwnedWireMessage& message, std::uint64_t sequence, std::size_t reservedPayloadBytes, std::size_t actualPayloadBytes, std::uint8_t seed)
{
    if (actualPayloadBytes == 0U || actualPayloadBytes > reservedPayloadBytes || reservedPayloadBytes > c_maxPayloadBytes)
    {
        return false;
    }

    Error error = Error::None;
    if (!PreparePcmMessage(1U, sequence, reservedPayloadBytes, message, error))
    {
        std::cerr << "PreparePcmMessage reserve failed: " << static_cast<int>(error) << '\n';
        return false;
    }
    for (std::size_t index = 0U; index < actualPayloadBytes; ++index)
    {
        message.m_bytes[c_headerBytes + index] = static_cast<std::uint8_t>(seed + (index * 13U));
    }
    if (actualPayloadBytes != reservedPayloadBytes && !PreparePcmMessage(1U, sequence, actualPayloadBytes, message, error))
    {
        std::cerr << "PreparePcmMessage finalize failed: " << static_cast<int>(error) << '\n';
        return false;
    }
    return true;
}

Message MakePcmMessage(std::uint64_t sequence, std::size_t payloadBytes, std::uint8_t seed)
{
    std::vector<std::uint8_t> payload(payloadBytes);
    for (std::size_t index = 0U; index < payloadBytes; ++index)
    {
        payload[index] = static_cast<std::uint8_t>(seed + (index * 13U));
    }
    return MakeMessage(MessageType::Pcm, sequence, std::move(payload));
}

bool CreatePipePair(OwnedHandle& server, OwnedHandle& client, DWORD serverOutputBufferBytes = 4096U)
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
        serverOutputBufferBytes,
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

void TestReaderRecyclesPayloadBuffersAcrossFourMessages()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for repeated reader buffer reuse");
    if (!server || !client)
    {
        return;
    }

    PipeMessageReader reader;
    Message received{};
    std::array<const std::uint8_t*, 4U> payloadBuffers{};
    for (std::uint64_t index = 0U; index < payloadBuffers.size(); ++index)
    {
        const std::uint64_t sequence = 60U + index;
        const Message inbound = MakePcmMessage(sequence, 1024U, static_cast<std::uint8_t>(0x21U + index));
        std::vector<std::uint8_t> wire;
        if (!Check(Encode(inbound, wire), "repeated PCM frame encodes") ||
            !Check(reader.Begin(server.Get(), 1000U) == PipeResult::Pending, "reader begins the next repeated frame") ||
            !Check(WriteAll(client.Get(), wire), "repeated PCM frame reaches reader"))
        {
            reader.CancelAndDrain();
            return;
        }
        if (!Check(AwaitReader(reader, received, 1000U) == PipeResult::Success, "reader completes a repeated PCM frame"))
        {
            reader.CancelAndDrain();
            return;
        }
        Check(received.m_header.m_sequence == sequence && received.m_payload == inbound.m_payload, "reused reader output preserves each frame");
        payloadBuffers[static_cast<std::size_t>(index)] = received.m_payload.data();
    }

    Check(payloadBuffers[0U] == payloadBuffers[2U], "reader alternates back to the first bounded payload allocation");
    Check(payloadBuffers[1U] == payloadBuffers[3U], "reader alternates back to the second bounded payload allocation");
    Check(received.m_payload.capacity() <= c_maxPayloadBytes, "reader never adopts output capacity above the protocol payload bound");

    received.m_payload.reserve(c_maxPayloadBytes + 64U);
    Check(received.m_payload.capacity() > c_maxPayloadBytes, "test output holds an oversized caller buffer");
    const Message finalInbound = MakePcmMessage(64U, 64U, 0xCCU);
    std::vector<std::uint8_t> finalWire;
    if (!Check(Encode(finalInbound, finalWire), "final bounded PCM frame encodes") ||
        !Check(reader.Begin(server.Get(), 1000U) == PipeResult::Pending, "reader begins after oversized output storage is supplied") ||
        !Check(WriteAll(client.Get(), finalWire), "final bounded PCM frame reaches reader") ||
        !Check(AwaitReader(reader, received, 1000U) == PipeResult::Success, "reader completes after oversized output storage is supplied"))
    {
        reader.CancelAndDrain();
        return;
    }
    Check(received.m_payload == finalInbound.m_payload, "oversized output fallback still delivers the exact payload");
    Check(received.m_payload.capacity() <= c_maxPayloadBytes, "reader declines to retain oversized caller capacity");
}

void TestPreparedWriterExactWireOwnershipAndBufferReuse()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for prepared writer ownership test");
    if (!server || !client)
    {
        return;
    }

    AsyncPipeMessageWriter writer;
    const Message expectedFirst = MakePcmMessage(71U, 1920U, 0x37U);
    std::vector<std::uint8_t> expectedFirstWire;
    if (!Check(Encode(expectedFirst, expectedFirstWire), "expected first PCM wire frame encodes"))
    {
        return;
    }

    const std::uint8_t* firstAllocation = nullptr;
    {
        OwnedWireMessage frame{};
        Error validationError = Error::None;
        if (!Check(PreparePcmWire(frame, 71U, 4096U, 1920U, 0x37U), "first PCM payload is written directly after the reserved header") ||
            !Check(ValidateWireMessage(frame.m_bytes.data(), frame.m_bytes.size(), validationError), "prepared frame validates before transfer"))
        {
            return;
        }
        firstAllocation = frame.m_bytes.data();
        if (!Check(writer.BeginPrepared(server.Get(), 1000U, frame) == PipeResult::Pending, "prepared frame transfers to the async writer"))
        {
            return;
        }
        Check(frame.m_bytes.empty(), "writer takes the complete framed allocation without retaining the caller object");
        frame.m_bytes.assign(128U, 0xE7U);
        Check(frame.m_bytes.size() == 128U, "caller can mutate its returned spare while the write is pending");
    }

    std::vector<std::uint8_t> receivedWire;
    if (!Check(ReadAll(client.Get(), expectedFirstWire.size(), receivedWire), "client receives the owned prepared wire frame") ||
        !Check(receivedWire == expectedFirstWire, "owned prepared writer emits byte-for-byte canonical framing") ||
        !Check(AwaitWriter(writer, 1000U) == PipeResult::Success, "prepared writer reports completion after the input object was destroyed"))
    {
        writer.CancelAndDrain();
        return;
    }

    const Message expectedSecond = MakePcmMessage(72U, 640U, 0xA1U);
    std::vector<std::uint8_t> expectedSecondWire;
    if (!Check(Encode(expectedSecond, expectedSecondWire), "expected second PCM wire frame encodes"))
    {
        return;
    }
    OwnedWireMessage second{};
    if (!Check(PreparePcmWire(second, 72U, 640U, 640U, 0xA1U), "second PCM payload is prepared"))
    {
        return;
    }
    const std::uint8_t* secondAllocation = second.m_bytes.data();
    if (!Check(writer.BeginPrepared(server.Get(), 1000U, second) == PipeResult::Pending, "writer returns its prior frame allocation while taking the next message"))
    {
        return;
    }
    Check(second.m_bytes.empty(), "writer clears the returned spare frame before returning it");
    Check(second.m_bytes.data() == firstAllocation, "the original wire allocation returns by identity instead of being copied");
    Check(second.m_bytes.capacity() >= expectedFirstWire.size(), "the returned wire allocation retains bounded reusable capacity");
    second.m_bytes.assign(96U, 0xE7U);
    Check(second.m_bytes.size() == 96U, "mutating a returned spare does not alter the in-flight writer allocation");

    if (!Check(ReadAll(client.Get(), expectedSecondWire.size(), receivedWire), "client receives the reused writer frame") ||
        !Check(receivedWire == expectedSecondWire, "reused prepared writer emits only the next frame") ||
        !Check(AwaitWriter(writer, 1000U) == PipeResult::Success, "reused prepared writer completes the next frame"))
    {
        writer.CancelAndDrain();
        return;
    }

    const Message expectedThird = MakePcmMessage(73U, 3000U, 0x77U);
    std::vector<std::uint8_t> expectedThirdWire;
    if (!Check(Encode(expectedThird, expectedThirdWire), "expected third PCM wire frame encodes") ||
        !Check(PreparePcmWire(second, 73U, 3000U, 3000U, 0x77U), "producer prepares a later frame in the returned allocation") ||
        !Check(second.m_bytes.data() == firstAllocation, "producer reuses the original allocation without reallocating") ||
        !Check(writer.BeginPrepared(server.Get(), 1000U, second) == PipeResult::Pending, "writer takes the re-prepared frame allocation"))
    {
        writer.CancelAndDrain();
        return;
    }
    Check(second.m_bytes.empty() && second.m_bytes.data() == secondAllocation, "writer returns its previous allocation for another reuse cycle");
    if (!Check(ReadAll(client.Get(), expectedThirdWire.size(), receivedWire), "client receives the third recycled wire frame") ||
        !Check(receivedWire == expectedThirdWire, "recycled allocation emits exact new frame bytes") ||
        !Check(AwaitWriter(writer, 1000U) == PipeResult::Success, "writer completes the recycled frame"))
    {
        writer.CancelAndDrain();
    }
}

void TestPreparedWriterHandlesPipeBackpressureAndMalformedInput()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client, 64U), "small-buffer pipe pair opens for maximum prepared frame backpressure test");
    if (!server || !client)
    {
        return;
    }

    AsyncPipeMessageWriter writer;
    const Message expectedMaximum = MakePcmMessage(81U, c_maxPayloadBytes, 0x4BU);
    std::vector<std::uint8_t> expectedMaximumWire;
    OwnedWireMessage maximum{};
    if (!Check(Encode(expectedMaximum, expectedMaximumWire), "maximum expected PCM wire frame encodes") ||
        !Check(PreparePcmWire(maximum, 81U, c_maxPayloadBytes, c_maxPayloadBytes, 0x4BU), "maximum PCM payload is prepared in bounded storage") ||
        !Check(writer.BeginPrepared(server.Get(), 5000U, maximum) == PipeResult::Pending, "maximum prepared frame starts across a smaller named-pipe buffer"))
    {
        writer.CancelAndDrain();
        return;
    }

    Check(maximum.m_bytes.empty(), "maximum prepared buffer transfers to the writer while bytes are in flight");
    std::vector<std::uint8_t> receivedHeader;
    if (!Check(ReadAll(client.Get(), c_headerBytes, receivedHeader), "client reads the header while the maximum write is backpressured") ||
        !Check(std::equal(receivedHeader.begin(), receivedHeader.end(), expectedMaximumWire.begin()), "backpressured write starts with the exact wire header") ||
        !Check(writer.Advance() == PipeResult::Pending, "writer remains pending after only the header prefix is drained"))
    {
        writer.CancelAndDrain();
        return;
    }

    std::vector<std::uint8_t> receivedPayload;
    if (!Check(ReadAll(client.Get(), expectedMaximumWire.size() - c_headerBytes, receivedPayload), "client drains the remaining payload through pipe backpressure") ||
        !Check(std::equal(receivedPayload.begin(), receivedPayload.end(), expectedMaximumWire.begin() + static_cast<std::ptrdiff_t>(c_headerBytes)), "backpressured prepared write preserves every payload byte") ||
        !Check(AwaitWriter(writer, 5000U) == PipeResult::Success, "writer completes after the pipe buffer is drained"))
    {
        writer.CancelAndDrain();
        return;
    }

    OwnedWireMessage malformed{};
    if (!Check(PreparePcmWire(malformed, 82U, 64U, 64U, 0x22U), "malformed frame starts from a valid prepared message"))
    {
        return;
    }
    malformed.m_bytes.push_back(0U);
    const std::vector<std::uint8_t> originalMalformed = malformed.m_bytes;
    Check(writer.BeginPrepared(server.Get(), 1000U, malformed) == PipeResult::InvalidMessage, "prepared writer rejects trailing bytes before taking ownership");
    Check(malformed.m_bytes == originalMalformed, "malformed input remains byte-for-byte unchanged");
}

void TestPreparedWriterCancellationDrainsBeforeBufferReuse()
{
    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for prepared writer cancellation test");
    if (!server || !client)
    {
        return;
    }

    AsyncPipeMessageWriter writer;
    OwnedWireMessage large{};
    if (!Check(PreparePcmWire(large, 91U, c_maxPayloadBytes, c_maxPayloadBytes, 0x5CU), "large cancellation frame is prepared"))
    {
        return;
    }
    const std::uint8_t* largeAllocation = large.m_bytes.data();
    if (!Check(writer.BeginPrepared(server.Get(), 10000U, large) == PipeResult::Pending, "large prepared write becomes pending against an unread pipe"))
    {
        return;
    }
    Check(large.m_bytes.empty(), "writer owns the in-flight buffer before cancellation");
    large.m_bytes.assign(64U, 0xEFU);
    writer.CancelAndDrain();
    Check(writer.Advance() == PipeResult::IoError, "cancelled prepared writer returns to idle after draining");

    server.Reset();
    client.Reset();
    OwnedHandle freshServer;
    OwnedHandle freshClient;
    Check(CreatePipePair(freshServer, freshClient), "fresh pipe pair opens after prepared write cancellation");
    if (!freshServer || !freshClient)
    {
        return;
    }

    OwnedWireMessage recovery{};
    if (!Check(PreparePcmWire(recovery, 92U, 256U, 256U, 0xC3U), "small recovery frame is prepared") ||
        !Check(writer.BeginPrepared(freshServer.Get(), 1000U, recovery) == PipeResult::Pending, "writer safely reuses storage after cancel and drain"))
    {
        writer.CancelAndDrain();
        return;
    }
    Check(recovery.m_bytes.empty() && recovery.m_bytes.data() == largeAllocation, "cancelled large allocation returns to the next caller after drain");
    Check(recovery.m_bytes.capacity() >= c_headerBytes + c_maxPayloadBytes, "cancelled buffer retains only protocol-bounded capacity");

    const Message expected = MakePcmMessage(92U, 256U, 0xC3U);
    std::vector<std::uint8_t> expectedWire;
    std::vector<std::uint8_t> receivedWire;
    if (!Check(Encode(expected, expectedWire), "recovery expected frame encodes") ||
        !Check(ReadAll(freshClient.Get(), expectedWire.size(), receivedWire), "fresh client receives the post-cancel frame") ||
        !Check(receivedWire == expectedWire, "post-cancel reuse writes the exact new frame") ||
        !Check(AwaitWriter(writer, 1000U) == PipeResult::Success, "post-cancel writer completes successfully"))
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
    TestReaderRecyclesPayloadBuffersAcrossFourMessages();
    TestPreparedWriterExactWireOwnershipAndBufferReuse();
    TestPreparedWriterHandlesPipeBackpressureAndMalformedInput();
    TestPreparedWriterCancellationDrainsBeforeBufferReuse();
    TestCancellationAndWriterTimeoutRecovery();

    if (failures != 0)
    {
        std::cerr << failures << " async transport test(s) failed\n";
        return 1;
    }
    std::cout << "Async audio IPC transport tests passed\n";
    return 0;
}
