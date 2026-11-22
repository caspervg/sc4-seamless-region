#pragma once

// The batch run as pure data: which cities to render, in which order, what happened to each,
// and its batch.json text. No game calls, unit tested (tests/BatchTests.cpp). Header only, so
// the tests need no change to their CMake file.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "region/RegionLayout.hpp"

namespace BatchQueue
{
    inline constexpr int kFormatVersion = 1;
    // A city whose render started this often without an end (the game stopped) is skipped.
    inline constexpr int kMaxAttempts = 2;

    enum class State
    {
        Running,    // active: a restart offers to continue it
        Done,       // every city was handled
        Cancelled,  // the player cancelled it
        Stopped,    // the player chose not to continue it after a restart
    };

    struct Entry
    {
        std::string key;
        std::string name;
        int cellX = 0, cellZ = 0, sizeX = 1, sizeZ = 1;
    };

    struct Result
    {
        std::string key;
        std::string name;
        std::string reason;    // failed and skipped only
        double seconds = 0.0;  // done only
    };

    struct Run
    {
        std::string runId;
        State state = State::Running;
        bool force = false;
        // Adds the transport layer to the earlier renders (CaptureRequest::addTransport).
        bool addTransport = false;
        // The render choices at the start, so a run that continues after a restart renders the
        // same: the transport layer (CaptureRequest::transport) and the highest zoom index
        // (-1: the setting of now, for files of older versions).
        bool transport = false;
        int maxZoom = -1;
        uint32_t rotations = 0xF;    // bit r: render rotation r (all four for files of older versions)
        Entry start;                 // the city the run started in, and ends in. Empty when regionView
        bool regionView = false;     // started in the region view: it ends there and renders every city
        std::vector<Entry> pending;  // in render order, the first is the next
        std::vector<Result> done;
        std::vector<Result> skipped;
        std::vector<Result> failed;
        std::map<std::string, int> attempts;  // renders started per city key
        std::string startedAt;                // local time text
        std::string finishedAt;
    };

    inline Entry ToEntry(const CityInfo& city)
    {
        return Entry{city.key, city.name, city.cellX, city.cellZ, city.sizeX, city.sizeZ};
    }

    // Moves the city with `key` to the front, the rest keeps its order.
    inline void MoveToFront(std::vector<Entry>& cities, const std::string& key)
    {
        if (const auto it = std::ranges::find(cities, key, &Entry::key); it != cities.end()) {
            std::rotate(cities.begin(), it, it + 1);
        }
    }

    // A new run from the region's cities. Unfounded cities and cities whose render is up to
    // date (isStale false, unless force) go to skipped. The rest goes largest first by `size`
    // (the save file size, about the memory a city load needs; ties in the order of `cities`):
    // the largest cities load while the 32 bit game has the most free address space, and the
    // smaller ones after them mostly fit in what it already holds. The start city stays first, it
    // is open already. Without a start city (nullptr: the region view) the run ends in the region
    // view.
    inline Run Build(const std::vector<CityInfo>& cities, const CityInfo* start, const bool force,
                     const std::function<bool(const CityInfo&)>& isStale, std::string runId, std::string startedAt,
                     const std::function<uint64_t(const CityInfo&)>& size = {})
    {
        Run run;
        run.runId = std::move(runId);
        run.startedAt = std::move(startedAt);
        run.force = force;
        run.regionView = !start;
        if (start) {
            run.start = ToEntry(*start);
        }
        std::vector<uint64_t> sizes;
        for (const CityInfo& city : cities) {
            if (!city.founded) {
                run.skipped.push_back({city.key, city.name, "Not founded", 0.0});
            }
            else if (!force && !isStale(city)) {
                run.skipped.push_back({city.key, city.name, "Up to date", 0.0});
            }
            else {
                run.pending.push_back(ToEntry(city));
                sizes.push_back(size ? size(city) : 0);
            }
        }
        std::vector<size_t> order(run.pending.size());
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::ranges::stable_sort(order, [&](const size_t a, const size_t b) { return sizes[a] > sizes[b]; });
        std::vector<Entry> sorted;
        sorted.reserve(order.size());
        for (const size_t i : order) {
            sorted.push_back(std::move(run.pending[i]));
        }
        run.pending = std::move(sorted);
        if (start) {
            MoveToFront(run.pending, run.start.key);
        }
        return run;
    }

    // Counts a started render of the first pending city. Call before the work that can crash.
    inline int BeginAttempt(Run& run, const std::string& key) { return ++run.attempts[key]; }

    inline void RemovePending(Run& run, const std::string& key)
    {
        std::erase_if(run.pending, [&](const Entry& e) { return e.key == key; });
    }

    inline void Complete(Run& run, const Entry& city, const double seconds)
    {
        RemovePending(run, city.key);
        run.done.push_back({city.key, city.name, std::string(), seconds});
    }

    inline void Fail(Run& run, const Entry& city, std::string reason)
    {
        RemovePending(run, city.key);
        run.failed.push_back({city.key, city.name, std::move(reason), 0.0});
    }

    // Before a run continues after a restart: cities that were started kMaxAttempts times
    // without an end fail, and the rest is ordered again from the city now open. Without an open
    // city (nullptr: the region view) the saved order stays; with one, that city comes first.
    inline void PrepareResume(Run& run, const Entry* current)
    {
        std::vector<Entry> crashed;
        for (const Entry& e : run.pending) {
            const auto it = run.attempts.find(e.key);
            if (it != run.attempts.end() && it->second >= kMaxAttempts) {
                crashed.push_back(e);
            }
        }
        for (const Entry& e : crashed) {
            Fail(run, e, "The game stopped twice while it rendered this city.");
        }
        if (current) {
            MoveToFront(run.pending, current->key);
        }
    }

    // Cities handled so far plus the ones left.
    inline int Total(const Run& run) { return static_cast<int>(run.done.size() + run.failed.size() + run.pending.size()); }

    inline const char* StateName(const State state)
    {
        switch (state) {
        case State::Running: return "running";
        case State::Done: return "done";
        case State::Cancelled: return "cancelled";
        case State::Stopped: return "stopped";
        }
        return "running";
    }

    // ---- batch.json ----------------------------------------------------------------------

    inline nlohmann::json EntryJson(const Entry& e)
    {
        return {{"key", e.key}, {"name", e.name}, {"cellX", e.cellX}, {"cellZ", e.cellZ}, {"sizeX", e.sizeX},
                {"sizeZ", e.sizeZ}};
    }

    inline nlohmann::json ResultsJson(const std::vector<Result>& results)
    {
        nlohmann::json array = nlohmann::json::array();
        for (const Result& r : results) {
            array.push_back({{"key", r.key}, {"name", r.name}, {"reason", r.reason}, {"seconds", r.seconds}});
        }
        return array;
    }

    inline std::string ToJson(const Run& run)
    {
        nlohmann::json pending = nlohmann::json::array();
        for (const Entry& e : run.pending) {
            pending.push_back(EntryJson(e));
        }
        const nlohmann::json j{
            {"formatVersion", kFormatVersion},
            {"runId", run.runId},
            {"state", StateName(run.state)},
            {"force", run.force},
            {"addTransport", run.addTransport},
            {"transport", run.transport},
            {"maxZoom", run.maxZoom},
            {"rotations", run.rotations},
            {"start", EntryJson(run.start)},
            {"regionView", run.regionView},
            {"pending", std::move(pending)},
            {"done", ResultsJson(run.done)},
            {"skipped", ResultsJson(run.skipped)},
            {"failed", ResultsJson(run.failed)},
            {"attempts", run.attempts},
            {"startedAt", run.startedAt},
            {"finishedAt", run.finishedAt},
        };
        return j.dump(2);
    }

    // Nullopt (and error, when not null) for text that is not a batch.json of this version.
    inline std::optional<Run> Parse(const std::string_view text, std::string* error = nullptr)
    {
        try {
            const nlohmann::json j = nlohmann::json::parse(text);
            if (j.at("formatVersion").get<int>() != kFormatVersion) {
                if (error) {
                    *error = "unknown formatVersion";
                }
                return std::nullopt;
            }
            const auto entry = [](const nlohmann::json& e) {
                return Entry{e.at("key").get<std::string>(), e.at("name").get<std::string>(), e.at("cellX").get<int>(),
                             e.at("cellZ").get<int>(),       e.at("sizeX").get<int>(),        e.at("sizeZ").get<int>()};
            };
            const auto results = [](const nlohmann::json& array) {
                std::vector<Result> out;
                for (const nlohmann::json& r : array) {
                    out.push_back({r.at("key").get<std::string>(), r.at("name").get<std::string>(),
                                   r.value("reason", std::string()), r.value("seconds", 0.0)});
                }
                return out;
            };
            Run run;
            run.runId = j.at("runId").get<std::string>();
            const std::string state = j.at("state").get<std::string>();
            if (state == "running") {
                run.state = State::Running;
            }
            else if (state == "done") {
                run.state = State::Done;
            }
            else if (state == "cancelled") {
                run.state = State::Cancelled;
            }
            else if (state == "stopped") {
                run.state = State::Stopped;
            }
            else {
                if (error) {
                    *error = "unknown state '" + state + "'";
                }
                return std::nullopt;
            }
            run.force = j.at("force").get<bool>();
            run.addTransport = j.value("addTransport", false);  // not in files of older versions
            run.transport = j.value("transport", false);
            run.maxZoom = j.value("maxZoom", -1);
            run.rotations = j.value("rotations", 0xFu) & 0xFu;
            run.rotations = run.rotations ? run.rotations : 0xFu;
            run.start = entry(j.at("start"));
            run.regionView = j.value("regionView", false);  // not in files of older versions
            for (const nlohmann::json& e : j.at("pending")) {
                run.pending.push_back(entry(e));
            }
            run.done = results(j.at("done"));
            run.skipped = results(j.at("skipped"));
            run.failed = results(j.at("failed"));
            run.attempts = j.at("attempts").get<std::map<std::string, int>>();
            run.startedAt = j.value("startedAt", std::string());
            run.finishedAt = j.value("finishedAt", std::string());
            return run;
        }
        catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }
            return std::nullopt;
        }
    }
}
