#pragma once

#include <spdlog/sinks/rotating_file_sink.h>

namespace logger = SKSE::log;

namespace AgencyEngine
{
    // Writes to Documents/My Games/Skyrim Special Edition/SKSE/AgencyEngine.log,
    // rotated so the two previous logs survive as AgencyEngine.log.1 and .log.2.
    //
    // Rotation happens both on open and on size. On open, because the question
    // after a CTD is "what did the run that crashed do", and the run after it is
    // the one that opens the log — truncating on launch throws away the only
    // copy. On size, because verbose tick logging left on overnight is otherwise
    // unbounded; 10 MB is a few hours of it and far more than any bug needs.
    //
    // The level is trace, unconditionally: every per-tick line in the Director
    // is *individually* gated on the "Verbose tick logging" setting instead, so
    // the switch lives in one obvious place rather than being split between a
    // spdlog level and a settings flag. Flush-on-trace costs a little I/O but
    // means a CTD never eats the last few lines — which are the ones that
    // explain the CTD.
    //
    // The thread id is in the pattern on purpose. Three threads write here (the
    // game's main thread, the Director, and SkyrimNet's LLM worker), and
    // "which thread logged this" is the first question worth asking when
    // ordering looks impossible.
    inline void SetupLog()
    {
        auto logsFolder = SKSE::log::log_directory();
        if (!logsFolder) {
            SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
        }

        const auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
        const auto logFilePath = *logsFolder / std::format("{}.log", pluginName);

        // max_files counts the *rotated* copies, so 2 keeps three files in all.
        constexpr std::size_t maxFileSize = 10 * 1024 * 1024;
        constexpr std::size_t maxFiles = 2;
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFilePath.string(), maxFileSize, maxFiles,
                                                                          /*rotate_on_open=*/true);
        auto log = std::make_shared<spdlog::logger>("log", std::move(sink));
        spdlog::set_default_logger(std::move(log));
        spdlog::set_level(spdlog::level::trace);
        spdlog::flush_on(spdlog::level::trace);
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] [t%t] %v");
    }

    // Long strings (context payloads, LLM responses) are logged whole when
    // verbose logging is on and elided otherwise — a 40-event context JSON is
    // several KB and would drown everything else.
    inline std::string Elide(const std::string& text, std::size_t limit = 220)
    {
        if (text.size() <= limit) {
            return text;
        }
        return std::format("{}... [{} chars total]", text.substr(0, limit), text.size());
    }

    // Collapses newlines so a multi-line payload stays one log record.
    inline std::string OneLine(std::string text)
    {
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        return text;
    }
}
