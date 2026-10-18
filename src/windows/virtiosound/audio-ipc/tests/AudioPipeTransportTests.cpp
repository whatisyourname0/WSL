// Copyright (C) Microsoft Corporation. All rights reserved.

#ifndef _WIN32
#error AudioPipeTransportTests require Windows.
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
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using wsl::virtio_sound::audio_ipc::Message;
using wsl::virtio_sound::audio_ipc::MessageType;
using wsl::virtio_sound::audio_ipc::c_maxPayloadBytes;
using wsl::virtio_sound::audio_ipc::windows::PipeMessageWriter;
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

void CancelAndDrainConnect(HANDLE pipe, OVERLAPPED& operation)
{
    (void)CancelIoEx(pipe, &operation);
    (void)WaitForSingleObject(operation.hEvent, INFINITE);
    DWORD ignoredBytes = 0U;
    (void)GetOverlappedResult(pipe, &operation, &ignoredBytes, FALSE);
}

bool CreatePipePair(OwnedHandle& server, OwnedHandle& client)
{
    static std::atomic<std::uint64_t> counter{};
    const std::uint64_t instance = counter.fetch_add(1U, std::memory_order_relaxed);
    const std::wstring pipeName = L"\\\\.\\pipe\\wsl-audio-ipc-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(instance);

    server.Reset(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
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

    client.Reset(CreateFileW(pipeName.c_str(), GENERIC_READ, 0U, nullptr, OPEN_EXISTING, 0U, nullptr));
    if (!client)
    {
        const DWORD openError = GetLastError();
        if (connectPending)
        {
            CancelAndDrainConnect(server.Get(), connectOperation);
        }
        std::cerr << "CreateFileW for pipe client failed: " << openError << '\n';
        return false;
    }

    if (connectPending)
    {
        if (WaitForSingleObject(connectEvent.Get(), 1000U) != WAIT_OBJECT_0)
        {
            CancelAndDrainConnect(server.Get(), connectOperation);
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

bool ReadExactly(HANDLE pipe, std::size_t byteCount, std::vector<std::uint8_t>& bytes)
{
    bytes.resize(byteCount);
    std::size_t offset = 0U;
    while (offset < byteCount)
    {
        const std::size_t remaining = byteCount - offset;
        const DWORD request = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD transferred = 0U;
        if (ReadFile(pipe, bytes.data() + offset, request, &transferred, nullptr) == FALSE || transferred == 0U)
        {
            std::cerr << "ReadFile failed: " << GetLastError() << '\n';
            return false;
        }
        offset += transferred;
    }
    return true;
}

std::vector<std::uint8_t> HelloWire(std::uint64_t sequence)
{
    std::vector<std::uint8_t> wire{
        0x57U, 0x41U, 0x53U, 0x31U, 0x02U, 0x00U, 0x01U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    for (std::size_t index = 0U; index < sizeof(sequence); ++index)
    {
        wire[24U + index] = static_cast<std::uint8_t>((sequence >> (index * 8U)) & 0xFFU);
    }
    return wire;
}

bool WriteAndCheckHello(PipeMessageWriter& writer, HANDLE server, HANDLE client, HANDLE stopEvent, std::uint64_t sequence)
{
    const Message hello = MakeMessage(MessageType::Hello, sequence);
    if (!Check(writer.Write(server, stopEvent, 1000U, hello) == PipeResult::Success, "writer sends Hello"))
    {
        return false;
    }

    std::vector<std::uint8_t> received;
    const std::vector<std::uint8_t> expected = HelloWire(sequence);
    if (!Check(ReadExactly(client, expected.size(), received), "client reads complete Hello frame"))
    {
        return false;
    }
    return Check(received == expected, "named pipe receives exact Hello wire bytes");
}

bool SendAndCheckHello(PipeMessageWriter& writer, HANDLE stopEvent)
{
    OwnedHandle server;
    OwnedHandle client;
    return CreatePipePair(server, client) && WriteAndCheckHello(writer, server.Get(), client.Get(), stopEvent, 1U);
}

Message LargePcmMessage()
{
    std::vector<std::uint8_t> payload(c_maxPayloadBytes, static_cast<std::uint8_t>(0x2AU));
    return MakeMessage(MessageType::Pcm, 2U, std::move(payload));
}

void TestRepeatedWritesAndTimeoutReuse()
{
    PipeMessageWriter writer;
    OwnedHandle repeatedServer;
    OwnedHandle repeatedClient;
    Check(CreatePipePair(repeatedServer, repeatedClient), "pipe pair opens for repeated writes");
    if (!repeatedServer || !repeatedClient)
    {
        return;
    }
    Check(
        WriteAndCheckHello(writer, repeatedServer.Get(), repeatedClient.Get(), nullptr, 1U),
        "first write succeeds on the pipe session");
    Check(
        WriteAndCheckHello(writer, repeatedServer.Get(), repeatedClient.Get(), nullptr, 2U),
        "second write reuses the writer and event on the same pipe session");

    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for timeout test");
    if (!server || !client)
    {
        return;
    }
    const Message largePcm = LargePcmMessage();
    Check(writer.Write(server.Get(), nullptr, 100U, largePcm) == PipeResult::Timeout, "blocked write times out and drains its operation");
    // A timeout can leave a partial frame in the old byte stream. The retry
    // uses a fresh pipe while retaining the same writer and event.
    client.Reset();
    server.Reset();
    Check(SendAndCheckHello(writer, nullptr), "writer safely reuses its event and buffer after timeout");
}

void TestCancellationDrainAndReuse()
{
    PipeMessageWriter writer;
    OwnedHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(static_cast<bool>(stopEvent), "stop event is created");
    if (!stopEvent)
    {
        return;
    }

    OwnedHandle server;
    OwnedHandle client;
    Check(CreatePipePair(server, client), "pipe pair opens for cancellation test");
    if (!server || !client)
    {
        return;
    }

    DWORD outboundQuotaBytes = 0U;
    if (GetNamedPipeInfo(client.Get(), nullptr, &outboundQuotaBytes, nullptr, nullptr) == FALSE)
    {
        std::cerr << "GetNamedPipeInfo on pipe client failed: " << GetLastError() << '\n';
        Check(false, "client reports the named pipe outbound quota");
        return;
    }
    if (!Check(outboundQuotaBytes != 0U, "client reports a nonzero named pipe outbound quota"))
    {
        return;
    }

    Message largePcm = LargePcmMessage();
    std::atomic<bool> started{};
    std::atomic<PipeResult> result{PipeResult::IoError};
    std::thread writeThread([&]() {
        started.store(true, std::memory_order_release);
        result.store(writer.Write(server.Get(), stopEvent.Get(), 10000U, largePcm), std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire))
    {
        Sleep(1U);
    }

    // This frame is larger than the pipe's outbound quota. Wait for the quota
    // to fill, then let the next overlapped write become pending before
    // requesting cancellation.
    const ULONGLONG deadline = GetTickCount64() + 2000U;
    DWORD availableBytes = 0U;
    while (GetTickCount64() < deadline)
    {
        if (PeekNamedPipe(client.Get(), nullptr, 0U, nullptr, &availableBytes, nullptr) == FALSE || availableBytes >= outboundQuotaBytes)
        {
            break;
        }
        Sleep(1U);
    }
    Sleep(25U);
    (void)SetEvent(stopEvent.Get());
    writeThread.join();

    Check(availableBytes >= outboundQuotaBytes, "large write filled the pipe before cancellation");
    Check(result.load(std::memory_order_acquire) == PipeResult::Cancelled, "stop event cancels and drains the pending write");

    client.Reset();
    server.Reset();
    Check(ResetEvent(stopEvent.Get()) != FALSE, "stop event resets for the next operation");
    Check(SendAndCheckHello(writer, stopEvent.Get()), "writer safely reuses its event and buffer after cancellation");
}

} // namespace

int main()
{
    TestRepeatedWritesAndTimeoutReuse();
    TestCancellationDrainAndReuse();

    if (failures != 0)
    {
        std::cerr << failures << " transport test(s) failed\n";
        return 1;
    }
    std::cout << "Audio IPC transport tests passed\n";
    return 0;
}
