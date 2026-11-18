#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

class Logger
{
public:
    static std::shared_ptr<spdlog::logger> Get();
    // userDir is the folder for the log file. Empty: Documents\SimCity 4.
    static void Initialize(const std::string& logName = "SC4SeamlessRegion",
                           const std::filesystem::path& userDir = {},
                           bool logToFile = true);
    // A path as UTF-8 text for log lines and the panel. std::filesystem::path::string()
    // throws for a name the ANSI code page cannot hold. This never throws.
    static std::string PathText(const std::filesystem::path& path) noexcept;
    static void SetLevel(spdlog::level::level_enum logLevel);
    static void Shutdown();

private:
    static std::shared_ptr<spdlog::logger> s_logger;
    static std::string s_logName;
    static bool s_initialized;
};

#define LOG_TRACE(...) Logger::Get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) Logger::Get()->debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::Get()->info(__VA_ARGS__)
#define LOG_WARN(...) Logger::Get()->warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::Get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) Logger::Get()->critical(__VA_ARGS__)

