// Copyright (C) Microsoft Corporation. All rights reserved.

#include "RxDoorbellLifecycle.hpp"
#include "GuestStopPreparation.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace wsl::virtio_sound::native::windows::detail;

namespace {
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool IsExactRxSpecification(const RxDoorbellSpecification& spec) noexcept
{
    return spec.c_barIndex == 0 && spec.c_barOffset == 0x30c && spec.c_triggerValue == 3 && spec.c_flags == 2;
}

void TestFallbackWhenEitherRequirementIsMissing()
{
    RxDoorbellLifecycle missingApi;
    unsigned int calls{};
    Require(
        !missingApi.TryRegister(false, true, [&](const auto&) {
            ++calls;
            return true;
        }),
        "registration ran without both API exports");
    Require(missingApi.State() == RxDoorbellState::Disabled && !missingApi.MustRetainEvent(), "missing API pair did not disable safely");

    RxDoorbellLifecycle missingEvent;
    Require(
        !missingEvent.TryRegister(true, false, [&](const auto&) {
            ++calls;
            return true;
        }),
        "registration ran without an event");
    Require(calls == 0, "disabled registration invoked the fake API");
}

void TestExactTupleAndSuccessfulUnregister()
{
    RxDoorbellLifecycle lifecycle;
    std::vector<char> order;
    Require(
        lifecycle.TryRegister(true, true, [&](const auto& spec) {
            order.push_back('R');
            return IsExactRxSpecification(spec);
        }),
        "exact RX doorbell tuple was not accepted");
    Require(lifecycle.Active() && lifecycle.MustRetainEvent(), "registered state did not retain the event");
    unsigned int duplicateCalls{};
    Require(
        !lifecycle.TryRegister(true, true, [&](const auto&) {
            ++duplicateCalls;
            return false;
        }),
        "repeated registration replaced an active doorbell");
    Require(duplicateCalls == 0 && lifecycle.Active(), "repeated registration obscured active cleanup state");
    Require(
        lifecycle.TryUnregister([&](const auto& spec) {
            order.push_back('U');
            return IsExactRxSpecification(spec);
        }),
        "exact RX doorbell tuple was not unregistered");
    Require(order == std::vector<char>{'R', 'U'}, "unregister did not follow register");
    Require(lifecycle.State() == RxDoorbellState::Unregistered && !lifecycle.MustRetainEvent(), "successful unregister retained the event");
}

void TestRegistrationFailureFallsBack()
{
    RxDoorbellLifecycle lifecycle;
    Require(!lifecycle.TryRegister(true, true, [](const auto&) { return false; }), "fake registration failure was reported as success");
    Require(lifecycle.State() == RxDoorbellState::RegistrationFailed, "registration failure state was lost");
    Require(!lifecycle.MustRetainEvent(), "failed registration unnecessarily pinned the event");
    Require(lifecycle.TryUnregister([](const auto&) { return false; }), "fallback path attempted to unregister a failed registration");
}

void TestUnregisterFailureRequiresPinning()
{
    RxDoorbellLifecycle lifecycle;
    Require(lifecycle.TryRegister(true, true, [](const auto&) { return true; }), "fake registration failed");
    Require(!lifecycle.TryUnregister([](const auto&) { return false; }), "fake unregister failure was reported as success");
    Require(lifecycle.State() == RxDoorbellState::UnregisterFailed, "unregister failure state was lost");
    Require(lifecycle.MustRetainEvent(), "uncertain unregister did not require event retention");
    Require(!lifecycle.TryUnregister([](const auto&) { return true; }), "uncertain unregister was automatically retried");
}

void TestPrepareWithoutRegistrationClosesAdmission()
{
    RxDoorbellLifecycle lifecycle;
    unsigned int unregisterCalls{};
    Require(
        lifecycle.PrepareForGuestStop([&](const auto&) {
            ++unregisterCalls;
            return true;
        }),
        "unattempted doorbell did not prepare for guest stop");
    Require(lifecycle.State() == RxDoorbellState::Prepared, "unattempted state was not frozen as prepared");
    Require(!lifecycle.TryRegister(true, true, [](const auto&) { return true; }), "doorbell registered after stop preparation");
    Require(
        lifecycle.PrepareForGuestStop([&](const auto&) {
            ++unregisterCalls;
            return true;
        }),
        "repeated prepare was not idempotent");
    Require(unregisterCalls == 0, "prepare without a registration called unregister");
    Require(lifecycle.CompleteAfterGuestStop(), "prepared inactive doorbell did not retire after VM exit");
    Require(
        lifecycle.State() == RxDoorbellState::Retired && !lifecycle.MustRetainEvent(),
        "retired inactive doorbell retained its event");
}

void TestPrepareRegisteredDoorbellUnregistersBeforeGuestStop()
{
    RxDoorbellLifecycle lifecycle;
    Require(lifecycle.TryRegister(true, true, [](const auto&) { return true; }), "fake registration failed");
    unsigned int unregisterCalls{};
    Require(
        lifecycle.PrepareForGuestStop([&](const auto& spec) {
            ++unregisterCalls;
            return IsExactRxSpecification(spec);
        }),
        "registered doorbell did not unregister during preparation");
    Require(
        lifecycle.State() == RxDoorbellState::Prepared && !lifecycle.MustRetainEvent(), "prepared doorbell retained its event");
    Require(
        lifecycle.PrepareForGuestStop([&](const auto&) {
            ++unregisterCalls;
            return true;
        }),
        "repeated successful preparation failed");
    Require(!lifecycle.TryRegister(true, true, [](const auto&) { return true; }), "prepared doorbell was registered again");
    Require(lifecycle.CompleteAfterGuestStop(), "prepared registration did not complete after VM exit");
    Require(unregisterCalls == 1, "prepare or final retirement repeated the unregister API call");
}

void TestPrepareFailureRetainsEventAndDoesNotRetryAfterExit()
{
    RxDoorbellLifecycle lifecycle;
    Require(lifecycle.TryRegister(true, true, [](const auto&) { return true; }), "fake registration failed");
    unsigned int unregisterCalls{};
    Require(
        !lifecycle.PrepareForGuestStop([&](const auto&) {
            ++unregisterCalls;
            return false;
        }),
        "failed pre-stop unregister was reported as prepared");
    Require(
        lifecycle.State() == RxDoorbellState::UnregisterFailed && lifecycle.MustRetainEvent(),
        "failed prepare did not retain event");
    Require(!lifecycle.CompleteAfterGuestStop(), "failed unregister was treated as clean after VM exit");
    Require(unregisterCalls == 1, "final retirement retried HDV unregister after VM exit");
}

void TestGuestStopWaitsForRegistrationThenUnregisters()
{
    RxDoorbellLifecycle lifecycle;
    std::mutex mutex;
    std::condition_variable changed;
    bool registrationEntered{};
    bool releaseRegistration{};
    bool registrationSucceeded{};
    unsigned int unregisterCalls{};

    std::thread registrar([&] {
        registrationSucceeded = lifecycle.TryRegister(true, true, [&](const auto&) {
            std::unique_lock lock{mutex};
            registrationEntered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return releaseRegistration; });
            return true;
        });
    });
    {
        std::unique_lock lock{mutex};
        changed.wait(lock, [&] { return registrationEntered; });
    }
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard lock{mutex};
            releaseRegistration = true;
        }
        changed.notify_all();
    });
    const bool prepared = lifecycle.PrepareForGuestStop([&](const auto&) {
        ++unregisterCalls;
        return true;
    });
    registrar.join();
    release.join();
    Require(registrationSucceeded && prepared, "prepare did not drain an in-flight registration");
    Require(
        unregisterCalls == 1 && lifecycle.State() == RxDoorbellState::Prepared, "in-flight registration escaped pre-stop cleanup");
    Require(
        !lifecycle.TryRegister(true, true, [](const auto&) { return true; }),
        "registration reopened after concurrent preparation");
    Require(lifecycle.CompleteAfterGuestStop(), "concurrently prepared doorbell did not retire cleanly");
}

void TestBoundedGuestStopRequestPolicy()
{
    using Result = GuestStopPreparationResult;
    std::vector<char> order;
    bool gotBoundedTimeout{};
    Result abortedFor = Result::RequestFailed;
    const auto timeout = GuestStopPreparation::Request(
        [&] {
            order.push_back('R');
            return true;
        },
        [&] {
            order.push_back('S');
            return true;
        },
        [&](std::uint32_t timeoutMilliseconds) {
            order.push_back('W');
            gotBoundedTimeout = timeoutMilliseconds == GuestStopPreparation::c_timeoutMilliseconds;
            return Result::TimedOut;
        },
        [&](Result reason) {
            order.push_back('A');
            abortedFor = reason;
        });
    Require(
        timeout == Result::TimedOut && gotBoundedTimeout && abortedFor == Result::TimedOut,
        "preparation wait did not propagate its bounded timeout or abort reason");
    Require(
        GuestStopPreparation::c_maximumWaitMilliseconds == 4000,
        "configured preparation and emergency-abort waits do not sum to four seconds");
    Require(order == std::vector<char>{'R', 'S', 'W', 'A'}, "timeout did not abort after request and wait, before returning");

    bool signaled{};
    bool waited{};
    bool aborted{};
    const auto resetFailure = GuestStopPreparation::Request(
        [] { return false; },
        [&] {
            signaled = true;
            return true;
        },
        [&](std::uint32_t) {
            waited = true;
            return Result::Prepared;
        },
        [&](Result) { aborted = true; });
    Require(
        resetFailure == Result::RequestFailed && !signaled && !waited && !aborted,
        "reset failure still signaled, waited, or aborted");
    const auto signalFailure = GuestStopPreparation::Request(
        [] { return true; },
        [] { return false; },
        [&](std::uint32_t) {
            waited = true;
            return Result::Prepared;
        },
        [&](Result) { aborted = true; });
    Require(
        signalFailure == Result::RequestFailed && !waited && !aborted,
        "request failure still waited or aborted without a successful signal");

    for (const auto reply : {Result::Prepared, Result::Rejected, Result::ChildExited})
    {
        aborted = false;
        const auto observed = GuestStopPreparation::Request(
            [] { return true; }, [] { return true; }, [reply](std::uint32_t) { return reply; }, [&](Result) { aborted = true; });
        Require(observed == reply && !aborted, "non-timeout preparation outcome was aborted or not propagated");
    }

    for (const auto reply : {Result::TimedOut, Result::WaitFailed})
    {
        aborted = false;
        Result perReplyAbortReason = Result::RequestFailed;
        const auto observed = GuestStopPreparation::Request(
            [] { return true; },
            [] { return true; },
            [reply](std::uint32_t) { return reply; },
            [&](Result reason) {
                aborted = true;
                perReplyAbortReason = reason;
            });
        Require(
            observed == reply && aborted && perReplyAbortReason == reply,
            "timeout or wait failure did not abort before returning");
    }

    aborted = false;
    const auto waitException = GuestStopPreparation::Request(
        [] { return true; },
        [] { return true; },
        [](std::uint32_t) -> Result { throw std::runtime_error("wait exception"); },
        [&](Result reason) { aborted = reason == Result::WaitFailed; });
    Require(waitException == Result::WaitFailed && aborted, "exception after signaling did not abort the child");
}
} // namespace

int main()
{
    try
    {
        TestFallbackWhenEitherRequirementIsMissing();
        TestExactTupleAndSuccessfulUnregister();
        TestRegistrationFailureFallsBack();
        TestUnregisterFailureRequiresPinning();
        TestPrepareWithoutRegistrationClosesAdmission();
        TestPrepareRegisteredDoorbellUnregistersBeforeGuestStop();
        TestPrepareFailureRetainsEventAndDoesNotRetryAfterExit();
        TestGuestStopWaitsForRegistrationThenUnregisters();
        TestBoundedGuestStopRequestPolicy();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "PASS: RX doorbell lifecycle and bounded guest-stop preparation policy\n";
    return EXIT_SUCCESS;
}
