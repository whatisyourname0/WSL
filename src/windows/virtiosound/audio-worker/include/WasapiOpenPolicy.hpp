// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <audioclient.h>

namespace wsl::virtio_sound::audio_worker {

enum class WasapiOpenFailure
{
    None,
    Initialize,
    FreshClientActivation,
    DefaultBufferInitialize,
    GetBufferSize,
    ZeroBufferCapacity,
};

struct WasapiOpenResult
{
    HRESULT m_hresult{E_FAIL};
    UINT32 m_endpointBufferFrames{};
    WasapiOpenFailure m_failure{WasapiOpenFailure::Initialize};
    bool m_usedDefaultBufferFallback{};

    bool Succeeded() const noexcept
    {
        return m_failure == WasapiOpenFailure::None && SUCCEEDED(m_hresult);
    }
};

// The adapter supplies the current IAudioClient operations so this policy can
// be tested without activating a device. The caller must already have validated
// the exact shared-mode format. In this fixed event-driven shared-mode call
// path, E_INVALIDARG (after exact format validation) and the explicit
// AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL duration error are eligible for retry;
// all other errors (including unsupported format, device, and access errors)
// are returned without changing the requested 100 ms reserve. A failed
// Initialize may leave the interface unusable, so the retry always uses a
// newly activated client and the documented shared-event (0, 0) buffer
// arguments.
template <typename Adapter>
WasapiOpenResult InitializeSharedEventCapture(Adapter& adapter, REFERENCE_TIME requestedBufferDuration) noexcept
{
    WasapiOpenResult openResult{};
    HRESULT result = adapter.Initialize(requestedBufferDuration, 0);
    if (result == E_INVALIDARG || result == AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL)
    {
        result = adapter.ActivateFresh();
        if (FAILED(result))
        {
            openResult.m_hresult = result;
            openResult.m_failure = WasapiOpenFailure::FreshClientActivation;
            return openResult;
        }

        openResult.m_usedDefaultBufferFallback = true;
        result = adapter.Initialize(0, 0);
        if (FAILED(result))
        {
            openResult.m_hresult = result;
            openResult.m_failure = WasapiOpenFailure::DefaultBufferInitialize;
            return openResult;
        }
    }
    else if (FAILED(result))
    {
        openResult.m_hresult = result;
        openResult.m_failure = WasapiOpenFailure::Initialize;
        return openResult;
    }

    result = adapter.GetBufferSize(&openResult.m_endpointBufferFrames);
    if (FAILED(result))
    {
        openResult.m_hresult = result;
        openResult.m_failure = WasapiOpenFailure::GetBufferSize;
        return openResult;
    }
    if (openResult.m_endpointBufferFrames == 0)
    {
        openResult.m_hresult = E_UNEXPECTED;
        openResult.m_failure = WasapiOpenFailure::ZeroBufferCapacity;
        return openResult;
    }

    openResult.m_hresult = S_OK;
    openResult.m_failure = WasapiOpenFailure::None;
    return openResult;
}

} // namespace wsl::virtio_sound::audio_worker
