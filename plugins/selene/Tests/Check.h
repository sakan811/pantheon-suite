// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

// Tiny assertion helper shared by the test executables.
namespace test
{
inline int failures = 0;

inline void check (bool ok, const std::string& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok) ++failures;
}

inline std::string fmt (const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start (args, format);
    std::vsnprintf (buffer, sizeof buffer, format, args);
    va_end (args);
    return buffer;
}

inline int finish (const char* name)
{
    std::printf ("\n%s %s (%d failure%s)\n", name, failures == 0 ? "PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
}
