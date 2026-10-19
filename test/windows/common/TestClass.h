/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TestClass.h

Abstract:

    TAEF test class declarations shared by standalone and WSL tests.

--*/

#pragma once

#include <WexTestClass.h>

#define WSL_TEST_CLASS_PROPERTIES \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"LxssManager.dll") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"LxssManagerProxyStub.dll") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslclient.dll") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslservice.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"WslServiceProxyStub.dll") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslhost.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslrelay.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslconfig.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wsl.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslg.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"msrdc.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"msal.wsl.proxy.exe") \
    TEST_CLASS_PROPERTY(L"BinaryUnderTest", L"wslcsession.exe")

#define WSL_TEST_CLASS(_name) \
    BEGIN_TEST_CLASS(_name) \
        WSL_TEST_CLASS_PROPERTIES \
    END_TEST_CLASS()

#define WSLC_TEST_CLASS(_name) \
    BEGIN_TEST_CLASS(_name) \
        WSL_TEST_CLASS_PROPERTIES \
        TEST_CLASS_PROPERTY(L"TestCategory", L"WSLC") \
    END_TEST_CLASS()
