// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

// VM-owned HDV process. A high-integrity VM worker cannot
// open the system-integrity service for VM_OPERATION/VM_WRITE/DUP_HANDLE.
// Keep the service security unchanged and host the device in a bounded child.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <wil/resource.h>
#include <wil/result.h>

#include "CaptureEventScheduler.hpp"
#include "CaptureHostTimingDiagnostics.hpp"
#include "HdvPciDeviceHost.hpp"
#include "WindowsCaptureSource.hpp"
#include "../../virtiosound/include/GuestStopPreparation.hpp"
#include "../../virtiosound/src/windows/AudioSourceTrace.hpp"

namespace wsl::virtio_sound::process_host {
constexpr std::uint32_t c_startupMagic = 0x48565341;
constexpr std::uint32_t c_startupVersion = 1;

struct ReadyPacket
{
    HRESULT m_result{E_FAIL};
    char m_detail[384]{};
};

struct StartupPacket
{
    std::uint32_t m_magic{c_startupMagic};
    std::uint32_t m_version{c_startupVersion};
    std::uint64_t m_userToken{};
    wchar_t m_endpoint[1025]{};
};

inline HANDLE ParseHandle(const wchar_t* text)
{
    wchar_t* end{};
    const auto value = _wcstoui64(text, &end, 10);
    THROW_HR_IF(E_INVALIDARG, !value || !end || *end);
    return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value));
}

inline void RequireSystemHighToken()
{
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    const auto user = wil::get_token_information<TOKEN_USER>(token.get());
    THROW_HR_IF(E_ACCESSDENIED, !IsWellKnownSid(user->User.Sid, WinLocalSystemSid));
    const auto label = wil::get_token_information<TOKEN_MANDATORY_LABEL>(token.get());
    const auto sid = label->Label.Sid;
    THROW_HR_IF(E_ACCESSDENIED, *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) != SECURITY_MANDATORY_HIGH_RID);
}

inline int Run(int argc, wchar_t** argv) noexcept
{
    ReadyPacket packet;
    wil::unique_handle output;
    try
    {
        THROW_HR_IF(E_INVALIDARG, argc != 11);
        output.reset(ParseHandle(argv[4]));
        RequireSystemHighToken();
        const auto stop = ParseHandle(argv[5]);
        const auto parent = ParseHandle(argv[6]);
        const auto prepareRequest = ParseHandle(argv[7]);
        const auto prepareSucceeded = ParseHandle(argv[8]);
        const auto prepareFailed = ParseHandle(argv[9]);
        wil::unique_handle configuration{ParseHandle(argv[10])};
        DWORD flags{};
        for (const auto handle : {output.get(), stop, parent, prepareRequest, prepareSucceeded, prepareFailed, configuration.get()})
        {
            THROW_IF_WIN32_BOOL_FALSE(GetHandleInformation(handle, &flags));
            THROW_HR_IF(E_ACCESSDENIED, !(flags & HANDLE_FLAG_INHERIT));
        }
        THROW_HR_IF(E_INVALIDARG, GetFileType(output.get()) != FILE_TYPE_PIPE);
        THROW_HR_IF(E_INVALIDARG, GetFileType(configuration.get()) != FILE_TYPE_PIPE);
        StartupPacket startup;
        DWORD received{};
        THROW_IF_WIN32_BOOL_FALSE(ReadFile(configuration.get(), &startup, sizeof(startup), &received, nullptr));
        configuration.reset();
        THROW_HR_IF(E_INVALIDARG, received != sizeof(startup) || startup.m_magic != c_startupMagic || startup.m_version != c_startupVersion);
        THROW_HR_IF(E_INVALIDARG, startup.m_endpoint[ARRAYSIZE(startup.m_endpoint) - 1] != 0);
        THROW_HR_IF(E_INVALIDARG, (startup.m_userToken != 0) != (startup.m_endpoint[0] != 0));
        GUID vm{}, instance{};
        // HCS names use the unbraced GUID form; CLSIDFromString requires braces.
        const std::wstring vmText = argv[2];
        THROW_HR_IF(E_INVALIDARG, vmText.size() != 36);
        const std::wstring bracedVm = L"{" + vmText + L"}";
        THROW_IF_FAILED(CLSIDFromString(bracedVm.c_str(), &vm));
        THROW_IF_FAILED(CLSIDFromString(argv[3], &instance));
        THROW_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        auto uninitialize = wil::scope_exit([] { CoUninitialize(); });
        using unique_hcs_system = wil::unique_any<HCS_SYSTEM, decltype(&HcsCloseComputeSystem), HcsCloseComputeSystem>;
        unique_hcs_system system;
        THROW_IF_FAILED(HcsOpenComputeSystem(argv[2], GENERIC_ALL, &system));
        auto captureWake = std::make_shared<native::windows::CaptureWakeSignal>();
        std::shared_ptr<native::windows::WindowsCaptureSource> source;
        auto shutdownSource = wil::scope_exit([&] {
            if (source)
            {
                source->Shutdown();
            }
        });
        if (startup.m_userToken)
        {
            wil::unique_handle userToken{reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(startup.m_userToken))};
            THROW_IF_WIN32_BOOL_FALSE(GetHandleInformation(userToken.get(), &flags));
            THROW_HR_IF(E_ACCESSDENIED, !(flags & HANDLE_FLAG_INHERIT));
            // The WASAPI worker stays in the original restricted user session.
            // Create authenticates an idle worker; only a guest START opens audio.
            wchar_t image[32768]{};
            const auto length = GetModuleFileNameW(nullptr, image, ARRAYSIZE(image));
            THROW_LAST_ERROR_IF(!length || length == ARRAYSIZE(image));
            const auto worker = std::filesystem::path(image).parent_path() / L"wsl-audio-worker.exe";
            source = native::windows::WindowsCaptureSource::Create(userToken.get(), worker.wstring(), startup.m_endpoint, captureWake);
        }
        constexpr GUID c_deviceClass{0xF1919B41, 0x6A33, 0x4C92, {0xA7, 0xF8, 0x39, 0x7E, 0xE4, 0xC5, 0xB1, 0xD2}};
        auto host = native::windows::HdvPciDeviceHost::CreateBorrowed(
            system.get(), c_deviceClass, instance, source, native::windows::HostOwnership::UnknownOrShared, captureWake);
        packet.m_result = S_OK;
        DWORD written{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(output.get(), &packet, sizeof(packet), &written, nullptr));
        THROW_HR_IF(E_FAIL, written != sizeof(packet));
        output.reset();
        using clock = std::chrono::steady_clock;
        const auto clockOrigin = clock::now();
        auto lastPump = clockOrigin;
        auto toNanoseconds = [&](clock::time_point time) {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(time - clockOrigin).count());
        };
        detail::CaptureEventScheduler scheduler;
        detail::CaptureHostTimingRing timingEvents;
        [[maybe_unused]] auto traceTimingEvents = wil::scope_exit([&]() noexcept {
            native::windows::detail::TraceAudioSource(L"HostPumpRing", timingEvents.Count(), timingEvents.TotalRecorded());
            for (std::size_t index = 0; index < timingEvents.Count(); ++index)
            {
                const auto* event = timingEvents.Oldest(index);
                if (!event)
                {
                    continue;
                }

                wchar_t detailText[190]{};
                // Compact field order: sequence, flags, wait start/requested/return/actual,
                // service start/duration, wait result/reason, frames, generation.
                if (swprintf_s(
                        detailText,
                        ARRAYSIZE(detailText),
                        L"HP %llx %x %llx %llx %llx %llx %llx %llx %x %x %d %llx",
                        static_cast<unsigned long long>(event->m_sequence),
                        static_cast<unsigned int>(event->m_flags),
                        static_cast<unsigned long long>(event->m_waitStartedAtNanoseconds),
                        static_cast<unsigned long long>(event->m_requestedWaitNanoseconds),
                        static_cast<unsigned long long>(event->m_waitReturnedAtNanoseconds),
                        static_cast<unsigned long long>(event->m_actualWaitNanoseconds),
                        static_cast<unsigned long long>(event->m_serviceStartedAtNanoseconds),
                        static_cast<unsigned long long>(event->m_serviceDurationNanoseconds),
                        static_cast<unsigned int>(event->m_waitResult),
                        static_cast<unsigned int>(event->m_waitReason),
                        static_cast<int>(event->m_frames),
                        static_cast<unsigned long long>(event->m_generation)) > 0)
                {
                    native::windows::detail::TraceAudioSource(detailText);
                }
            }
        });
        std::uint64_t eventWakes{}, deadlineWakes{}, emptyServices{}, deferredServices{}, completedFrames{};
        std::uint64_t sourceWakes{}, guestWakes{}, serviceNanoseconds{}, maximumServiceNanoseconds{};
        std::uint64_t lastGeneration{};
        auto lastWaitReason = native::CaptureWaitReason::Idle;
        native::windows::detail::TraceAudioSource(L"HostPumpClockOrigin", toNanoseconds(clock::now()));
        // A startup rescan covers state published before this loop began.
        captureWake->Notify(native::windows::CaptureWakeReason::GuestState);
        const HANDLE waits[]{stop, parent, prepareRequest, captureWake->Event(), captureWake->DataEvent()};
        for (;;)
        {
            const auto beforeWait = clock::now();
            const auto beforeWaitNanoseconds = toNanoseconds(beforeWait);
            const auto requestedWaitMilliseconds = scheduler.WaitTimeoutMilliseconds(beforeWaitNanoseconds);
            const bool deferSourceData = scheduler.DeferSourceData(beforeWaitNanoseconds);
            // Keep stop, parent lifetime, prepare, and source/guest state in
            // every wait. Only a data-only wake is held until the next rescan.
            const DWORD waitHandleCount = deferSourceData ? 4U : 5U;
            const DWORD wait = WaitForMultipleObjects(waitHandleCount, waits, FALSE, requestedWaitMilliseconds);
            const DWORD waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            const auto waitReturned = clock::now();
            const auto waitReturnedNanoseconds = toNanoseconds(waitReturned);
            auto recordWaitOnlyIfSlow = [&]() noexcept {
                (void)timingEvents.RecordIfSlow(
                    beforeWaitNanoseconds,
                    requestedWaitMilliseconds,
                    waitReturnedNanoseconds,
                    static_cast<std::uint32_t>(wait),
                    waitReturnedNanoseconds,
                    0,
                    static_cast<std::uint8_t>(lastWaitReason),
                    0,
                    lastGeneration);
            };
            if (wait == WAIT_OBJECT_0)
            {
                // The parent signals stop only after confirming guest shutdown.
                recordWaitOnlyIfSlow();
                break;
            }
            if (wait == WAIT_OBJECT_0 + 1)
            {
                // Parent death does not prove that the guest stopped. Close the
                // capture source during unwinding and let process exit release
                // the process-pinned callback root; never claim guest retirement.
                recordWaitOnlyIfSlow();
                if (source)
                {
                    source->Shutdown();
                }
                shutdownSource.release();
                return ERROR_PROCESS_ABORTED;
            }
            if (wait == WAIT_OBJECT_0 + 2)
            {
                // This runs on the pump thread, outside HDV callbacks. Normal
                // MMIO callbacks and capture remain live while the guest shuts
                // down; only the optional doorbell is disarmed.
                const HRESULT result = host->PrepareForGuestStop();
                const HANDLE acknowledgement = SUCCEEDED(result) ? prepareSucceeded : prepareFailed;
                if (!SetEvent(acknowledgement))
                {
                    native::windows::detail::TraceAudioSource(
                        L"HdvGuestStopPrepareAckFailure", static_cast<std::uint32_t>(result), GetLastError());
                }
                continue;
            }
            if (wait == WAIT_FAILED)
            {
                // Fail closed: unwinding shuts down the source, but leaves the
                // callback root pinned until this child process exits.
                recordWaitOnlyIfSlow();
                THROW_WIN32(waitError);
            }
            if (wait != WAIT_TIMEOUT && wait != WAIT_OBJECT_0 + 3 && (deferSourceData || wait != WAIT_OBJECT_0 + 4))
            {
                recordWaitOnlyIfSlow();
                THROW_HR(E_UNEXPECTED);
            }

            const auto pumpStarted = clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(pumpStarted - lastPump).count();
            // State events never grant sample credit. Always service readiness
            // and failures, even when the completion-rate deadline is not due.
            lastPump = pumpStarted;
            // While source data is deferred, an early state/guest wake may
            // service state without being allowed to discard the pending data
            // wake. Drain it only once data is eligible again or the scheduled
            // deadline itself caused this rescan; otherwise its signal must
            // survive reason consumption in case service leaves the scheduler
            // unarmed.
            if (!deferSourceData || wait == WAIT_TIMEOUT)
            {
                // A producer racing this drain either contributes to this
                // rescan or leaves its event signaled for the following one.
                (void)WaitForSingleObject(captureWake->DataEvent(), 0);
            }
            const auto reasons = captureWake->Consume();
            if (wait == WAIT_TIMEOUT)
            {
                ++deadlineWakes;
            }
            else
            {
                ++eventWakes;
            }
            sourceWakes += (reasons & 3U) != 0;
            guestWakes += (reasons & 12U) != 0;
            const auto startedAt = toNanoseconds(pumpStarted);
            const bool allowTransfer = scheduler.CanTransfer(startedAt, host->CapturePacingGeneration());
            const auto result = host->ServiceCapture(elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0, allowTransfer);
            scheduler.OnService(startedAt, result);
            const auto duration = toNanoseconds(clock::now()) - startedAt;
            (void)timingEvents.RecordIfSlow(
                beforeWaitNanoseconds,
                requestedWaitMilliseconds,
                waitReturnedNanoseconds,
                static_cast<std::uint32_t>(wait),
                startedAt,
                duration,
                static_cast<std::uint8_t>(result.m_waitReason),
                static_cast<std::int32_t>(result.m_frames),
                result.m_generation);
            lastGeneration = result.m_generation;
            lastWaitReason = result.m_waitReason;
            serviceNanoseconds += duration;
            maximumServiceNanoseconds = (std::max)(maximumServiceNanoseconds, duration);
            emptyServices += result.m_frames == 0;
            deferredServices += result.m_waitReason == native::CaptureWaitReason::PacingDeadline;
            completedFrames += static_cast<std::uint64_t>((std::max)(0, result.m_frames));
        }
        native::windows::detail::TraceAudioSource(L"CaptureWakeTotals", eventWakes, deadlineWakes);
        native::windows::detail::TraceAudioSource(L"CaptureWakeReasons", sourceWakes, guestWakes);
        native::windows::detail::TraceAudioSource(L"CaptureDoorbellActive", host->RxDoorbellRegistered() ? 1 : 0);
        native::windows::detail::TraceAudioSource(L"CaptureServiceEmpty", emptyServices, deferredServices);
        native::windows::detail::TraceAudioSource(L"CaptureServiceTime", serviceNanoseconds, maximumServiceNanoseconds);
        native::windows::detail::TraceAudioSource(L"CaptureServiceFrames", completedFrames);
        // The parent confirms guest shutdown before signaling stop. Keep the
        // existing ownership rule: do not tear down another WSL proxy registration.
        (void)host->GuestStoppedAndRetire();
        return 0;
    }
    catch (const std::exception& error)
    {
        strncpy_s(packet.m_detail, error.what(), _TRUNCATE);
    }
    catch (...)
    {
        packet.m_result = wil::ResultFromCaughtException();
    }
    if (output)
    {
        DWORD written{};
        (void)WriteFile(output.get(), &packet, sizeof(packet), &written, nullptr);
        output.reset();
    }
    return 1;
}

class HdvHostProcess final
{
public:
    static std::unique_ptr<HdvHostProcess> Create(
        const std::wstring& vm, const GUID& instance, HANDLE restrictedUserToken = nullptr, const std::wstring& endpoint = {})
    {
        auto host = std::make_unique<HdvHostProcess>();
        THROW_HR_IF(E_INVALIDARG, (restrictedUserToken != nullptr) != !endpoint.empty());
        StartupPacket startupPacket;
        THROW_HR_IF(E_INVALIDARG, endpoint.size() >= ARRAYSIZE(startupPacket.m_endpoint) || endpoint.find(L'\0') != std::wstring::npos);
        std::copy(endpoint.begin(), endpoint.end(), startupPacket.m_endpoint);
        wil::unique_handle processToken, childToken;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken));
        THROW_IF_WIN32_BOOL_FALSE(
            DuplicateTokenEx(processToken.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &childToken));
        const auto user = wil::get_token_information<TOKEN_USER>(childToken.get());
        THROW_HR_IF(E_ACCESSDENIED, !IsWellKnownSid(user->User.Sid, WinLocalSystemSid));
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        PSID highSid{};
        THROW_IF_WIN32_BOOL_FALSE(AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_HIGH_RID, 0, 0, 0, 0, 0, 0, 0, &highSid));
        auto freeSid = wil::scope_exit([&] { FreeSid(highSid); });
        TOKEN_MANDATORY_LABEL label{{highSid, SE_GROUP_INTEGRITY}};
        THROW_IF_WIN32_BOOL_FALSE(SetTokenInformation(childToken.get(), TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(highSid)));

        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        wil::unique_handle read, write, parent;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&read, &write, &attributes, 0));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0));
        host->m_stop.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        THROW_LAST_ERROR_IF(!host->m_stop);
        host->m_prepareRequest.reset(CreateEventW(&attributes, FALSE, FALSE, nullptr));
        THROW_LAST_ERROR_IF(!host->m_prepareRequest);
        host->m_prepareSucceeded.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        THROW_LAST_ERROR_IF(!host->m_prepareSucceeded);
        host->m_prepareFailed.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        THROW_LAST_ERROR_IF(!host->m_prepareFailed);
        THROW_IF_WIN32_BOOL_FALSE(
            DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE, TRUE, 0));
        wil::unique_handle configurationRead, configurationWrite, inheritedUserToken;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&configurationRead, &configurationWrite, &attributes, sizeof(StartupPacket)));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(configurationWrite.get(), HANDLE_FLAG_INHERIT, 0));
        std::vector<HANDLE> inherited{
            write.get(),
            host->m_stop.get(),
            parent.get(),
            host->m_prepareRequest.get(),
            host->m_prepareSucceeded.get(),
            host->m_prepareFailed.get(),
            configurationRead.get()};
        if (restrictedUserToken)
        {
            THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
                GetCurrentProcess(), restrictedUserToken, GetCurrentProcess(), &inheritedUserToken, TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, 0));
            inherited.push_back(inheritedUserToken.get());
            startupPacket.m_userToken = reinterpret_cast<ULONG_PTR>(inheritedUserToken.get());
        }
        DWORD sent{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(configurationWrite.get(), &startupPacket, sizeof(startupPacket), &sent, nullptr));
        THROW_HR_IF(E_FAIL, sent != sizeof(startupPacket));
        configurationWrite.reset();
        host->m_job = wsl::windows::common::helpers::CreateKillOnCloseJob();
        if (restrictedUserToken)
        {
            // The session-0 host and user-session WASAPI worker cannot share a
            // Windows job. Permit explicit breakaway only for this dedicated
            // host; the worker launch atomically assigns its own kill-on-close
            // job, whose last handle is owned by the host. Never silently break
            // arbitrary descendants away from lifecycle management.
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
            THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(host->m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
        }
        HANDLE jobHandle = host->m_job.get();
        auto processAttributes = wsl::windows::common::helpers::CreateProcThreadAttributeList(2);
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            processAttributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr));
        // Keep jobHandle alive until after the attribute list is destroyed.
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            processAttributes.get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &jobHandle, sizeof(jobHandle), nullptr, nullptr));
        wchar_t image[32768]{};
        const auto length = GetModuleFileNameW(nullptr, image, ARRAYSIZE(image));
        THROW_LAST_ERROR_IF(!length || length == ARRAYSIZE(image));
        wchar_t instanceText[40]{};
        THROW_HR_IF(E_FAIL, !StringFromGUID2(instance, instanceText, ARRAYSIZE(instanceText)));
        auto handleText = [](HANDLE h) { return std::to_wstring(reinterpret_cast<ULONG_PTR>(h)); };
        std::wstring command = L"\"" + std::wstring(image) + L"\" --virtio-sound-host " + vm + L" " + instanceText + L" " +
                               handleText(write.get()) + L" " + handleText(host->m_stop.get()) + L" " + handleText(parent.get()) +
                               L" " + handleText(host->m_prepareRequest.get()) + L" " + handleText(host->m_prepareSucceeded.get()) +
                               L" " + handleText(host->m_prepareFailed.get()) + L" " + handleText(configurationRead.get());
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = processAttributes.get();
        PROCESS_INFORMATION process{};
        THROW_IF_WIN32_BOOL_FALSE(CreateProcessAsUserW(
            childToken.get(),
            image,
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process));
        host->m_child.reset(process.hProcess);
        wil::unique_handle thread{process.hThread};
        auto killOnFailure = wil::scope_exit([&] { TerminateProcess(host->m_child.get(), 1); });
        // PROC_THREAD_ATTRIBUTE_JOB_LIST assigns the suspended process before
        // CreateProcessAsUserW returns. Verify the expected job before allowing
        // the child to execute; a failure here cannot leave an unbounded host.
        BOOL inHostJob = FALSE;
        THROW_IF_WIN32_BOOL_FALSE(IsProcessInJob(host->m_child.get(), host->m_job.get(), &inHostJob));
        THROW_HR_IF(E_UNEXPECTED, !inHostJob);
        THROW_LAST_ERROR_IF(ResumeThread(thread.get()) == static_cast<DWORD>(-1));
        write.reset();
        configurationRead.reset();
        inheritedUserToken.reset();
        const auto deadline = GetTickCount64() + 30000;
        ReadyPacket packet;
        for (;;)
        {
            DWORD available{};
            THROW_IF_WIN32_BOOL_FALSE(PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr));
            if (available >= sizeof(packet))
            {
                break;
            }
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), GetTickCount64() >= deadline);
            if (WaitForSingleObject(host->m_child.get(), 20) != WAIT_TIMEOUT)
            {
                // A fast child failure may write its diagnostic and exit between
                // PeekNamedPipe and the wait; consume that final packet first.
                THROW_IF_WIN32_BOOL_FALSE(PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr));
                if (available >= sizeof(packet))
                {
                    break;
                }
                DWORD exitCode{};
                THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(host->m_child.get(), &exitCode));
                THROW_HR_MSG(E_FAIL, "HDV child exited before readiness, code=0x%08x", exitCode);
            }
        }
        DWORD received{};
        THROW_IF_WIN32_BOOL_FALSE(ReadFile(read.get(), &packet, sizeof(packet), &received, nullptr));
        THROW_HR_IF(E_FAIL, received != sizeof(packet));
        packet.m_detail[sizeof(packet.m_detail) - 1] = 0;
        THROW_HR_IF_MSG(packet.m_result, packet.m_result < 0, "HDV child: %hs", packet.m_detail);
        killOnFailure.release();
        return host;
    }

    // Must be called only after the parent has observed confirmed guest exit.
    // Without this confirmation, destruction uses the job as an emergency
    // abort and the child skips GuestStoppedAndRetire.
    void ConfirmGuestStopped() noexcept
    {
        m_guestStoppedConfirmed.store(true, std::memory_order_release);
    }

    HRESULT PrepareForGuestStop() noexcept
    {
        bool expected = false;
        if (!m_prepareAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }

        DWORD requestError = ERROR_GEN_FAILURE;
        DWORD waitError = ERROR_GEN_FAILURE;
        const auto outcome = native::windows::detail::GuestStopPreparation::Request(
            [&] {
                if (!ResetEvent(m_prepareSucceeded.get()))
                {
                    requestError = GetLastError();
                    return false;
                }
                if (!ResetEvent(m_prepareFailed.get()))
                {
                    requestError = GetLastError();
                    return false;
                }
                return true;
            },
            [&] {
                if (!SetEvent(m_prepareRequest.get()))
                {
                    requestError = GetLastError();
                    return false;
                }
                return true;
            },
            [&](std::uint32_t timeoutMilliseconds) {
                const HANDLE waits[]{m_prepareSucceeded.get(), m_prepareFailed.get(), m_child.get()};
                const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, timeoutMilliseconds);
                if (wait == WAIT_OBJECT_0)
                {
                    return native::windows::detail::GuestStopPreparationResult::Prepared;
                }
                if (wait == WAIT_OBJECT_0 + 1)
                {
                    return native::windows::detail::GuestStopPreparationResult::Rejected;
                }
                if (wait == WAIT_OBJECT_0 + 2)
                {
                    return native::windows::detail::GuestStopPreparationResult::ChildExited;
                }
                if (wait == WAIT_TIMEOUT)
                {
                    return native::windows::detail::GuestStopPreparationResult::TimedOut;
                }
                waitError = GetLastError();
                return native::windows::detail::GuestStopPreparationResult::WaitFailed;
            },
            [&](native::windows::detail::GuestStopPreparationResult abortReason) noexcept {
                AbortUnacknowledgedGuestStopPreparation(abortReason);
            });

        HRESULT result = S_OK;
        switch (outcome)
        {
        case native::windows::detail::GuestStopPreparationResult::Prepared:
            break;
        case native::windows::detail::GuestStopPreparationResult::Rejected:
            result = E_FAIL; // Child logged the exact HdvUnregisterDoorbell HRESULT.
            break;
        case native::windows::detail::GuestStopPreparationResult::RequestFailed:
            result = HRESULT_FROM_WIN32(requestError);
            break;
        case native::windows::detail::GuestStopPreparationResult::TimedOut:
            result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            break;
        case native::windows::detail::GuestStopPreparationResult::ChildExited:
            result = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
            break;
        case native::windows::detail::GuestStopPreparationResult::WaitFailed:
            result = HRESULT_FROM_WIN32(waitError);
            break;
        case native::windows::detail::GuestStopPreparationResult::AlreadyRequested:
            result = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
            break;
        }
        native::windows::detail::TraceAudioSource(
            L"HdvGuestStopPrepareParent", static_cast<std::uint32_t>(outcome), static_cast<std::uint32_t>(result));
        return result;
    }

    ~HdvHostProcess()
    {
        if (!m_guestStoppedConfirmed.load(std::memory_order_acquire))
        {
            // Emergency abort: there is no proof that the guest stopped, so do
            // not ask the child to claim retirement. The callback root remains
            // process-pinned until job closure terminates the child. A stop-
            // preparation timeout uses this same owned-job abort while shutdown
            // is still pending; neither path guarantees clean unregister.
            m_job.reset();
            return;
        }

        if (!m_stop || !SetEvent(m_stop.get()))
        {
            // A failed graceful signal falls back to the bounded job abort.
            m_job.reset();
            return;
        }

        if (m_child && WaitForSingleObject(m_child.get(), 10000) != WAIT_OBJECT_0)
        {
            // The guest did stop, but the HDV child did not retire in time.
            // Closing the kill-on-close job is the explicit emergency fallback.
            m_job.reset();
        }
    }

private:
    void AbortUnacknowledgedGuestStopPreparation(native::windows::detail::GuestStopPreparationResult reason) noexcept
    {
        // Drop a request that may still be queued, then close only this
        // HdvHostProcess's kill-on-close job. Do not signal the child's normal
        // stop event: the guest has not been confirmed stopped, so it must not
        // claim clean retirement or a successful doorbell unregister.
        DWORD requestResetError = ERROR_SUCCESS;
        if (m_prepareRequest && !ResetEvent(m_prepareRequest.get()))
        {
            requestResetError = GetLastError();
        }
        m_job.reset();

        DWORD waitResult = WAIT_FAILED;
        DWORD waitError = ERROR_INVALID_HANDLE;
        DWORD childExitCode = STILL_ACTIVE;
        if (m_child)
        {
            waitResult = WaitForSingleObject(m_child.get(), native::windows::detail::GuestStopPreparation::c_abortChildWaitMilliseconds);
            waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            if (waitResult == WAIT_OBJECT_0 && !GetExitCodeProcess(m_child.get(), &childExitCode))
            {
                waitError = GetLastError();
            }
        }

        native::windows::detail::TraceAudioSource(
            L"HdvGuestStopPrepareEmergencyAbort", static_cast<std::uint32_t>(reason), static_cast<std::uint32_t>(waitResult));
        native::windows::detail::TraceAudioSource(L"HdvGuestStopPrepareEmergencyAbortDetails", requestResetError, waitError);
        native::windows::detail::TraceAudioSource(L"HdvGuestStopPrepareEmergencyAbortExitCode", childExitCode);
    }

    wil::unique_handle m_job;
    wil::unique_handle m_stop;
    wil::unique_handle m_prepareRequest;
    wil::unique_handle m_prepareSucceeded;
    wil::unique_handle m_prepareFailed;
    wil::unique_handle m_child;
    std::atomic_bool m_prepareAttempted{};
    std::atomic_bool m_guestStoppedConfirmed{};
};
} // namespace wsl::virtio_sound::process_host
