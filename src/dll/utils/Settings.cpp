#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <string>

#include "Logger.h"
#include "mini/ini.h"

namespace
{
    constexpr auto kSectionName = "SC4SeamlessRegion";

    std::string ToLower(std::string value)
    {
        std::ranges::transform(value, value.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    bool ParseLogLevel(const std::string& value, spdlog::level::level_enum& out)
    {
        const std::string normalized = ToLower(value);
        if (normalized == "trace") { out = spdlog::level::trace; return true; }
        if (normalized == "debug") { out = spdlog::level::debug; return true; }
        if (normalized == "info") { out = spdlog::level::info; return true; }
        if (normalized == "warn" || normalized == "warning") { out = spdlog::level::warn; return true; }
        if (normalized == "error") { out = spdlog::level::err; return true; }
        if (normalized == "critical") { out = spdlog::level::critical; return true; }
        if (normalized == "off") { out = spdlog::level::off; return true; }
        return false;
    }

    const char* LogLevelName(const spdlog::level::level_enum level)
    {
        switch (level) {
        case spdlog::level::trace: return "trace";
        case spdlog::level::debug: return "debug";
        case spdlog::level::warn: return "warn";
        case spdlog::level::err: return "error";
        case spdlog::level::critical: return "critical";
        case spdlog::level::off: return "off";
        default: return "info";
        }
    }

    bool ParseBool(const std::string& value, bool& out)
    {
        const std::string normalized = ToLower(value);
        if (normalized == "true" || normalized == "1" || normalized == "yes") { out = true; return true; }
        if (normalized == "false" || normalized == "0" || normalized == "no") { out = false; return true; }
        return false;
    }

    bool ParseInt(const std::string& value, int& out)
    {
        const char* begin = value.data();
        const char* end = begin + value.size();
        int parsed = 0;
        const auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec != std::errc() || ptr != end) {
            return false;
        }
        out = parsed;
        return true;
    }

    struct Reader
    {
        mINI::INIMap<std::string>& section;
        const std::filesystem::path& path;

        void Invalid(const char* key) const
        {
            LOG_WARN("Invalid {} in {}, using the default", key, Logger::PathText(path));
        }

        void Bool(const char* key, bool& field) const
        {
            bool value = field;
            if (section.has(key)) {
                if (ParseBool(section.get(key), value)) {
                    field = value;
                }
                else {
                    Invalid(key);
                }
            }
        }

        void Int(const char* key, int& field, const int min, const int max) const
        {
            if (!section.has(key)) {
                return;
            }
            int value = field;
            if (!ParseInt(section.get(key), value) || value < min || value > max) {
                Invalid(key);
                return;
            }
            field = value;
        }
    };
}

void Settings::Load(const std::filesystem::path& settingsFilePath)
{
    const uint32_t revision = revision_;
    *this = Settings();
    revision_ = revision + 1;

    try {
        const mINI::INIFile file(settingsFilePath);
        mINI::INIStructure ini;
        if (!file.read(ini) || !ini.has(kSectionName)) {
            return;
        }
        auto& section = ini[kSectionName];
        const Reader r{section, settingsFilePath};

        if (section.has("LogLevel")) {
            spdlog::level::level_enum level = logLevel;
            if (ParseLogLevel(section.get("LogLevel"), level)) {
                logLevel = level;
            }
            else {
                r.Invalid("LogLevel");
            }
        }
        r.Bool("LogToFile", logToFile);
        r.Bool("StartWindowVisible", startWindowVisible);
        r.Bool("Enabled", enabled);
        r.Bool("ShowNeighbors", showNeighbors);
        r.Bool("ShowFrontNeighbors", showFrontNeighbors);
        r.Bool("HoverTint", hoverTint);
        r.Bool("ClickToSwitch", clickToSwitch);
        r.Bool("RenderOnSave", renderOnSave);
        r.Bool("CaptureAtNoon", captureAtNoon);
        r.Bool("CaptureWaterGlare", captureWaterGlare);
        r.Bool("CaptureSnapshotFlag", captureSnapshotFlag);
        r.Int("MaxRenderZoom", maxRenderZoom, kMinRenderZoom, kMaxRenderZoom);
        r.Int("TileCacheMB", tileCacheMB, kMinTileCacheMB, kMaxTileCacheMB);
        r.Int("UploadTilesPerFrame", uploadTilesPerFrame, kMinUploadTiles, kMaxUploadTiles);
        r.Int("UploadMsPerFrame", uploadMsPerFrame, kMinUploadMs, kMaxUploadMs);
        r.Int("Zoom45BandMetres", zoom45BandMetres, kMinBandMetres, kMaxBandMetres);
        r.Int("WebpQuality", webpQuality, kMinWebpQuality, kMaxWebpQuality);
        r.Int("MapQuality", mapQuality, kMinWebpQuality, kMaxWebpQuality);
        r.Int("MapCacheMB", mapCacheMB, kMinMapCacheMB, kMaxMapCacheMB);
        r.Int("MapUpscaleLevels", mapUpscaleLevels, kMinMapUpscale, kMaxMapUpscale);
    }
    catch (const std::exception& e) {
        LOG_ERROR("Failed to read settings from {}: {}", Logger::PathText(settingsFilePath), e.what());
        *this = Settings();
        revision_ = revision + 1;
    }
}

bool Settings::Save(const std::filesystem::path& settingsFilePath) const
{
    try {
        const mINI::INIFile file(settingsFilePath);
        mINI::INIStructure ini;
        file.read(ini);  // a missing file is fine, write() creates it
        auto& s = ini[kSectionName];
        const auto b = [](const bool v) { return std::string(v ? "true" : "false"); };
        s["LogLevel"] = LogLevelName(logLevel);
        s["LogToFile"] = b(logToFile);
        s["StartWindowVisible"] = b(startWindowVisible);
        s["Enabled"] = b(enabled);
        s["ShowNeighbors"] = b(showNeighbors);
        s["ShowFrontNeighbors"] = b(showFrontNeighbors);
        s["HoverTint"] = b(hoverTint);
        s["ClickToSwitch"] = b(clickToSwitch);
        s["RenderOnSave"] = b(renderOnSave);
        s["CaptureAtNoon"] = b(captureAtNoon);
        s["CaptureWaterGlare"] = b(captureWaterGlare);
        s["CaptureSnapshotFlag"] = b(captureSnapshotFlag);
        s["MaxRenderZoom"] = std::to_string(maxRenderZoom);
        s["TileCacheMB"] = std::to_string(tileCacheMB);
        s["UploadTilesPerFrame"] = std::to_string(uploadTilesPerFrame);
        s["UploadMsPerFrame"] = std::to_string(uploadMsPerFrame);
        s["Zoom45BandMetres"] = std::to_string(zoom45BandMetres);
        s["WebpQuality"] = std::to_string(webpQuality);
        s["MapQuality"] = std::to_string(mapQuality);
        s["MapCacheMB"] = std::to_string(mapCacheMB);
        s["MapUpscaleLevels"] = std::to_string(mapUpscaleLevels);
        if (!file.write(ini)) {
            LOG_ERROR("Failed to write settings to {}", Logger::PathText(settingsFilePath));
            return false;
        }
        LOG_INFO("Saved settings to {}", Logger::PathText(settingsFilePath));
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Failed to write settings to {}: {}", Logger::PathText(settingsFilePath), e.what());
        return false;
    }
}
