// Copyright (C) Microsoft Corporation. All rights reserved.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TestClass.h"
#include "../../../src/windows/virtiosound/src/windows/JobBoundProcessStartup.hpp"

#include <Windows.h>
#include <LogController.h>
#include <wil/resource.h>

#include <array>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr DWORD c_suiteTimeoutMilliseconds = 60'000U;
constexpr DWORD c_runnerTerminationCode = 0xE0000001U;

void ModuleAnchor()
{
}

std::wstring GetModuleDirectory()
{
    const auto address = reinterpret_cast<LPCWSTR>(&ModuleAnchor);
    HMODULE module = nullptr;
    constexpr DWORD moduleFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(moduleFlags, address, &module) == FALSE)
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetModuleHandleExW failed");
    }

    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
    {
        throw std::runtime_error("GetModuleFileNameW failed or returned a truncated path");
    }

    std::wstring directory(path.data(), length);
    const auto separator = directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        throw std::runtime_error("The TAEF DLL path has no directory component");
    }
    directory.resize(separator);
    return directory;
}

void Fail(_In_ const std::wstring& message)
{
    WEX::Logging::Log::Comment(message.c_str());
    VERIFY_FAIL(message.c_str());
}

void RunSuite(_In_ const wchar_t* executableName)
{
    try
    {
        const auto directory = GetModuleDirectory();
        const auto executable = directory + L"\\" + executableName;
        const DWORD attributes = GetFileAttributesW(executable.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
        {
            Fail(L"Missing virtio-sound suite executable beside the TAEF DLL: " + executable);
            return;
        }

        wil::unique_handle job{CreateJobObjectW(nullptr, nullptr)};
        if (!job)
        {
            Fail(L"CreateJobObjectW failed with Win32 error " + std::to_wstring(GetLastError()));
            return;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == FALSE)
        {
            Fail(L"SetInformationJobObject failed with Win32 error " + std::to_wstring(GetLastError()));
            return;
        }

        wsl::virtio_sound::native::windows::detail::JobBoundProcessStartup startup(job.get());
        auto& startInfo = startup.Info();
        startInfo.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startInfo.StartupInfo.wShowWindow = SW_HIDE;
        std::wstring commandLine = L"\"" + executable + L"\"";
        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        PROCESS_INFORMATION information{};
        const DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                                    wsl::virtio_sound::native::windows::detail::JobBoundProcessStartup::RequiredCreationFlags();
        if (CreateProcessW(
                executable.c_str(),
                mutableCommandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                creationFlags,
                nullptr,
                directory.c_str(),
                &startInfo.StartupInfo,
                &information) == FALSE)
        {
            Fail(L"CreateProcessW failed with Win32 error " + std::to_wstring(GetLastError()) + L": " + executable);
            return;
        }

        wil::unique_handle process{information.hProcess};
        wil::unique_handle thread{information.hThread};
        const DWORD waitResult = WaitForSingleObject(process.get(), c_suiteTimeoutMilliseconds);
        if (waitResult != WAIT_OBJECT_0)
        {
            const DWORD waitError = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            (void)TerminateJobObject(job.get(), c_runnerTerminationCode);
            job.reset();
            if (WaitForSingleObject(process.get(), 5000U) == WAIT_TIMEOUT)
            {
                (void)TerminateProcess(process.get(), c_runnerTerminationCode);
                (void)WaitForSingleObject(process.get(), 5000U);
            }
            Fail(
                waitError == ERROR_TIMEOUT
                    ? L"Virtio-sound suite timed out after 60 seconds; its process job was terminated: " + executable
                    : L"Waiting for virtio-sound suite failed with Win32 error " + std::to_wstring(waitError) + L": " + executable);
            return;
        }

        DWORD exitCode = 0U;
        if (GetExitCodeProcess(process.get(), &exitCode) == FALSE)
        {
            Fail(L"GetExitCodeProcess failed with Win32 error " + std::to_wstring(GetLastError()) + L": " + executable);
            return;
        }
        job.reset(); // Reap any grandchildren when the suite process exits.
        if (exitCode != 0U)
        {
            Fail(L"Virtio-sound suite exited with code " + std::to_wstring(exitCode) + L": " + executable);
        }
    }
    catch (const std::exception& exception)
    {
        std::wstring message = L"Virtio-sound suite runner failed: ";
        for (const auto character : std::string(exception.what()))
        {
            message.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
        }
        Fail(message);
    }
}

} // namespace

namespace VirtioSoundUnitTests {

class VirtioSoundUnitTests
{
    WSL_TEST_CLASS(VirtioSoundUnitTests)

    TEST_METHOD(RxDoorbellLifecycleUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_rx_doorbell_lifecycle_tests.exe");
    }
    TEST_METHOD(EventSchedulerUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_event_scheduler_tests.exe");
    }
    TEST_METHOD(SchedulerSimulationUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_scheduler_simulation_tests.exe");
    }
    TEST_METHOD(HostTimingDiagnosticsUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_host_timing_diagnostics_tests.exe");
    }
    TEST_METHOD(DeviceDiagnosticsUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_diagnostics_tests.exe");
    }
    TEST_METHOD(DeviceCoreUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_tests.exe");
    }
    TEST_METHOD(PcmSourceLifecycleUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_pcm_source_lifecycle_tests.exe");
    }
    TEST_METHOD(PcmRingUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_pcm_ring_tests.exe");
    }
    TEST_METHOD(PcmRingCoreIntegrationUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_pcm_ring_core_integration_tests.exe");
    }
    TEST_METHOD(CaptureWakeDeferralUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_capture_wake_deferral_tests.exe");
    }
    TEST_METHOD(RestrictedUserTokenUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_restricted_user_token_tests.exe");
    }
    TEST_METHOD(WorkerPipeSecurityUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_worker_pipe_security_tests.exe");
    }
    TEST_METHOD(HdvLifecycleUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_hdv_lifecycle_tests.exe");
    }
    TEST_METHOD(AudioWorkerLifecycleUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_audio_worker_tests.exe");
    }
    TEST_METHOD(WasapiInitialPacketPolicyUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_wasapi_initial_packet_policy_tests.exe");
    }
    TEST_METHOD(WasapiCaptureTimingUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_wasapi_capture_timing_tests.exe");
    }
    TEST_METHOD(WasapiOpenPolicyUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_wasapi_open_policy_tests.exe");
    }
    TEST_METHOD(AudioIpcCodecUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_audio_ipc_tests.exe");
    }
    TEST_METHOD(AudioPipeTransportUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_audio_pipe_transport_tests.exe");
    }
    TEST_METHOD(AsyncPipeTransportUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_async_pipe_tests.exe");
    }
    TEST_METHOD(AudioWorkerSessionUnitTest)
    {
        RunSuite(L"wsl_virtio_sound_audio_worker_session_tests.exe");
    }
};

} // namespace VirtioSoundUnitTests
