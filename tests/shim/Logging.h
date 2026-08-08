#pragma once

// The logging shim.
//
// include/Logging.h is spdlog and SKSE::log, neither of which exists outside
// the game process. The ledger logs on every eviction and every decision, so
// compiling it for a test needs *something* under the `logger::` name — this is
// that something, and it deliberately throws the lines away rather than
// printing them. Tests that assert on log text are tests of the wording, and
// the wording is allowed to change.
//
// This directory is ahead of include/ on the test target's include path, so
// `#include "Logging.h"` from src/PendingImpulse.cpp finds this file.

#include <algorithm>
#include <format>
#include <string>

namespace logger
{
    // std::format_string rather than a bare const char*: it keeps the compiler
    // checking every call site's format string against its arguments, which is
    // most of what the real logger would have caught.
    template <class... Args>
    void trace(std::format_string<Args...>, Args&&...)
    {
    }
    template <class... Args>
    void debug(std::format_string<Args...>, Args&&...)
    {
    }
    template <class... Args>
    void info(std::format_string<Args...>, Args&&...)
    {
    }
    template <class... Args>
    void warn(std::format_string<Args...>, Args&&...)
    {
    }
    template <class... Args>
    void error(std::format_string<Args...>, Args&&...)
    {
    }
}

namespace AgencyEngine
{
    inline std::string Elide(const std::string& text, std::size_t limit = 220)
    {
        return text.size() <= limit ? text : text.substr(0, limit);
    }

    inline std::string OneLine(std::string text)
    {
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        return text;
    }
}
