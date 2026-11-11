// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WasapiOpenPolicy.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using wsl::virtio_sound::audio_worker::InitializeSharedEventCapture;
using wsl::virtio_sound::audio_worker::WasapiOpenFailure;

constexpr REFERENCE_TIME c_preferredBufferDuration = 1000000;
constexpr UINT32 c_expectedBufferFrames = 4800;

enum class Operation
{
    Initialize,
    ActivateFresh,
    GetBufferSize,
};

class FakeAdapter final
{
public:
    HRESULT Initialize(REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity) noexcept
    {
        m_operations.push_back(Operation::Initialize);
        m_arguments.emplace_back(bufferDuration, periodicity);
        const std::size_t callIndex = m_arguments.size() - 1;
        return callIndex < m_initializeResults.size() ? m_initializeResults[callIndex] : S_OK;
    }

    HRESULT ActivateFresh() noexcept
    {
        m_operations.push_back(Operation::ActivateFresh);
        return m_activationResult;
    }

    HRESULT GetBufferSize(UINT32* bufferFrames) noexcept
    {
        m_operations.push_back(Operation::GetBufferSize);
        *bufferFrames = m_bufferFrames;
        return m_bufferSizeResult;
    }

    std::vector<HRESULT> m_initializeResults;
    std::vector<std::pair<REFERENCE_TIME, REFERENCE_TIME>> m_arguments;
    std::vector<Operation> m_operations;
    HRESULT m_activationResult{S_OK};
    HRESULT m_bufferSizeResult{S_OK};
    UINT32 m_bufferFrames{c_expectedBufferFrames};
};

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireOperations(const FakeAdapter& adapter, std::vector<Operation> expected, const char* message)
{
    Require(adapter.m_operations == expected, message);
}

void TestPreferredReserveSucceedsUnchanged()
{
    FakeAdapter adapter;
    const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
    Require(result.Succeeded(), "a successful preferred-duration open must succeed");
    Require(!result.m_usedDefaultBufferFallback, "successful 100 ms opens must not use fallback");
    Require(result.m_endpointBufferFrames == c_expectedBufferFrames, "the returned capacity must be the actual adapter capacity");
    Require(adapter.m_arguments.size() == 1, "success must initialize only once");
    Require(
        adapter.m_arguments[0] == std::pair<REFERENCE_TIME, REFERENCE_TIME>{c_preferredBufferDuration, 0},
        "success must preserve the 100 ms duration and zero periodicity");
    RequireOperations(adapter, {Operation::Initialize, Operation::GetBufferSize}, "success must not reactivate the client");
}

void TestInvalidArgumentUsesFreshClientAndDocumentedDefault()
{
    FakeAdapter adapter;
    adapter.m_initializeResults = {E_INVALIDARG, S_OK};
    const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
    Require(result.Succeeded(), "an eligible duration-argument rejection may succeed with default buffering");
    Require(result.m_usedDefaultBufferFallback, "the result must record the default-buffer fallback");
    Require(result.m_endpointBufferFrames == c_expectedBufferFrames, "fallback must retain the actual capacity");
    Require(adapter.m_arguments.size() == 2, "eligible rejection must be followed by one retry");
    Require(
        adapter.m_arguments[0] == std::pair<REFERENCE_TIME, REFERENCE_TIME>{c_preferredBufferDuration, 0},
        "the first attempt must preserve the preferred reserve");
    Require(
        adapter.m_arguments[1] == std::pair<REFERENCE_TIME, REFERENCE_TIME>{0, 0},
        "the retry must use documented shared-event defaults");
    RequireOperations(
        adapter,
        {Operation::Initialize, Operation::ActivateFresh, Operation::Initialize, Operation::GetBufferSize},
        "the failed client must be replaced before retrying");
}

void TestDurationMismatchUsesFreshClientAndDocumentedDefault()
{
    FakeAdapter adapter;
    adapter.m_initializeResults = {AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL, S_OK};
    const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
    Require(result.Succeeded(), "the documented event buffer-duration mismatch may retry with shared defaults");
    Require(result.m_usedDefaultBufferFallback, "the duration-mismatch fallback must be recorded");
    Require(adapter.m_arguments.size() == 2, "duration mismatch must be followed by one retry");
    Require(
        adapter.m_arguments[1] == std::pair<REFERENCE_TIME, REFERENCE_TIME>{0, 0},
        "the duration-mismatch retry must use documented shared-event defaults");
    RequireOperations(
        adapter,
        {Operation::Initialize, Operation::ActivateFresh, Operation::Initialize, Operation::GetBufferSize},
        "the duration-mismatch path must activate a fresh client before retrying");
}

void TestFreshActivationFailureStopsRetry()
{
    FakeAdapter adapter;
    adapter.m_initializeResults = {E_INVALIDARG};
    adapter.m_activationResult = E_ACCESSDENIED;
    const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
    Require(!result.Succeeded(), "fresh activation failure must fail open");
    Require(result.m_hresult == E_ACCESSDENIED, "fresh activation HRESULT must be preserved");
    Require(result.m_failure == WasapiOpenFailure::FreshClientActivation, "fresh activation failure must be classified");
    RequireOperations(adapter, {Operation::Initialize, Operation::ActivateFresh}, "failed activation must not retry or query capacity");
}

void TestOnlyInvalidArgumentCanTriggerFallback()
{
    for (const HRESULT rejected : {AUDCLNT_E_UNSUPPORTED_FORMAT, AUDCLNT_E_DEVICE_INVALIDATED, E_ACCESSDENIED})
    {
        FakeAdapter adapter;
        adapter.m_initializeResults = {rejected};
        const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
        Require(!result.Succeeded(), "unsupported-format, device, and access errors must fail open");
        Require(result.m_hresult == rejected, "a nonretryable HRESULT must be preserved");
        Require(result.m_failure == WasapiOpenFailure::Initialize, "a nonretryable error must be classified as initialization failure");
        RequireOperations(adapter, {Operation::Initialize}, "nonretryable failures must not reactivate or query capacity");
    }
}

void TestDefaultBufferRetryFailureIsReturned()
{
    FakeAdapter adapter;
    adapter.m_initializeResults = {E_INVALIDARG, AUDCLNT_E_DEVICE_INVALIDATED};
    const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
    Require(!result.Succeeded(), "a failed default-buffer retry must fail open");
    Require(result.m_hresult == AUDCLNT_E_DEVICE_INVALIDATED, "the retry HRESULT must be preserved");
    Require(result.m_failure == WasapiOpenFailure::DefaultBufferInitialize, "the retry failure must be classified");
    Require(result.m_usedDefaultBufferFallback, "the retry attempt must be recorded");
    RequireOperations(
        adapter,
        {Operation::Initialize, Operation::ActivateFresh, Operation::Initialize},
        "a failed retry must not query buffer capacity");
}

void TestBufferSizeQueryFailureIsFatal()
{
    FakeAdapter adapter;
    adapter.m_bufferSizeResult = AUDCLNT_E_DEVICE_INVALIDATED;
    const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
    Require(!result.Succeeded(), "a failed capacity query must fail open because the watchdog cannot be bounded");
    Require(result.m_hresult == AUDCLNT_E_DEVICE_INVALIDATED, "the GetBufferSize HRESULT must be preserved");
    Require(result.m_failure == WasapiOpenFailure::GetBufferSize, "capacity-query failure must be classified");
    RequireOperations(adapter, {Operation::Initialize, Operation::GetBufferSize}, "capacity-query failure must not trigger fallback");
}

void TestZeroBufferCapacityIsFatal()
{
    FakeAdapter adapter;
    adapter.m_bufferFrames = 0;
    const auto result = InitializeSharedEventCapture(adapter, c_preferredBufferDuration);
    Require(!result.Succeeded(), "zero capacity must fail open because the watchdog cannot be bounded");
    Require(result.m_hresult == E_UNEXPECTED, "zero capacity must return an explicit failure HRESULT");
    Require(result.m_failure == WasapiOpenFailure::ZeroBufferCapacity, "zero capacity must be distinguished from query failure");
    RequireOperations(adapter, {Operation::Initialize, Operation::GetBufferSize}, "zero capacity must not trigger fallback");
}

} // namespace

int main()
{
    try
    {
        TestPreferredReserveSucceedsUnchanged();
        TestInvalidArgumentUsesFreshClientAndDocumentedDefault();
        TestDurationMismatchUsesFreshClientAndDocumentedDefault();
        TestFreshActivationFailureStopsRetry();
        TestOnlyInvalidArgumentCanTriggerFallback();
        TestDefaultBufferRetryFailureIsReturned();
        TestBufferSizeQueryFailureIsFatal();
        TestZeroBufferCapacityIsFatal();
        std::cout << "WASAPI open policy tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WASAPI open policy test failure: " << error.what() << '\n';
        return 1;
    }
}
