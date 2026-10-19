// Copyright (C) Microsoft Corporation. All rights reserved.

#include "WasapiCaptureTiming.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;
using wsl::virtio_sound::audio_worker::CaptureNotificationWaitTimeout;
using wsl::virtio_sound::audio_worker::c_captureBufferReserve;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestReserveIsOneHundredMilliseconds()
{
    Require(c_captureBufferReserve == 100ms, "the requested capture reserve should be 100 ms");
}

void TestReportedEndpointBufferSetsHalfBufferWatchdog()
{
    Require(
        CaptureNotificationWaitTimeout(std::chrono::milliseconds::max(), 1056U, 48000U) == 11ms,
        "a 1056-frame 48 kHz endpoint should use the floored half-buffer timeout");
    Require(
        CaptureNotificationWaitTimeout(std::chrono::milliseconds::max(), 4800U, 48000U) == 50ms,
        "a 100 ms endpoint reserve should cap its half-buffer watchdog at 50 ms");
}

void TestUnknownEndpointUsesFiniteFallback()
{
    Require(
        CaptureNotificationWaitTimeout(std::chrono::milliseconds::max(), 0U, 48000U) == 10ms,
        "a missing buffer size should use the finite fallback");
    Require(
        CaptureNotificationWaitTimeout(std::chrono::milliseconds::max(), 1056U, 0U) == 10ms,
        "a missing sample rate should use the finite fallback");
}

void TestCallerCanShortenButNotExtendWatchdog()
{
    Require(
        CaptureNotificationWaitTimeout(5ms, 1056U, 48000U) == 5ms,
        "a shorter caller timeout should remain in effect");
    Require(
        CaptureNotificationWaitTimeout(500ms, 1056U, 48000U) == 11ms,
        "a longer caller timeout should be bounded by the reported endpoint buffer");
    Require(
        CaptureNotificationWaitTimeout(100ms, 0U, 0U) == 10ms,
        "a known request should still be bounded when endpoint details are unknown");
}

void TestNonpositiveCallerTimeoutRetriesImmediately()
{
    Require(
        CaptureNotificationWaitTimeout(0ms, 1056U, 48000U) == 0ms,
        "a zero caller timeout should request an immediate retry");
    Require(
        CaptureNotificationWaitTimeout(-1ms, 1056U, 48000U) == 0ms,
        "a negative caller timeout should request an immediate retry");
}

void TestExtremeEndpointValuesRemainBounded()
{
    constexpr auto maximum = (std::numeric_limits<std::uint32_t>::max)();
    Require(
        CaptureNotificationWaitTimeout(std::chrono::milliseconds::max(), maximum, 1U) == 50ms,
        "extreme endpoint values should not overflow the timeout calculation");
    Require(
        CaptureNotificationWaitTimeout(std::chrono::milliseconds::max(), 1U, maximum) == 1ms,
        "a sub-millisecond half-buffer duration should clamp to one millisecond");
}

} // namespace

int main()
{
    try
    {
        TestReserveIsOneHundredMilliseconds();
        TestReportedEndpointBufferSetsHalfBufferWatchdog();
        TestUnknownEndpointUsesFiniteFallback();
        TestCallerCanShortenButNotExtendWatchdog();
        TestNonpositiveCallerTimeoutRetriesImmediately();
        TestExtremeEndpointValuesRemainBounded();
        std::cout << "WASAPI capture timing tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WASAPI capture timing test failure: " << error.what() << '\n';
        return 1;
    }
}
