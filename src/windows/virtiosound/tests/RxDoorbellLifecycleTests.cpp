// Copyright (C) Microsoft Corporation. All rights reserved.

#include "RxDoorbellLifecycle.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
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
} // namespace

int main()
{
    try
    {
        TestFallbackWhenEitherRequirementIsMissing();
        TestExactTupleAndSuccessfulUnregister();
        TestRegistrationFailureFallsBack();
        TestUnregisterFailureRequiresPinning();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "PASS: RX doorbell tuple, fallback, unregister, and pinning policy\n";
    return EXIT_SUCCESS;
}
