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

    // Twice the city centre, in cells, so the distances stay whole numbers.
    inline int64_t Distance2(const Entry& a, int ax2, int az2)
    {
        const int64_t dx = 2LL * a.cellX + a.sizeX - ax2;
        const int64_t dz = 2LL * a.cellZ + a.sizeZ - az2;
        return dx * dx + dz * dz;
    }

    // Orders the cities as a nearest-first walk from the city `from`: each next city is the one
    // closest to the last. A city with the key of `from` comes first. Ties go to the smaller
    // (cellZ, cellX), so the order is the same every time.
    // ponytail: greedy walk, O(n^2). A region has at most a few hundred cities.
    inline void Order(std::vector<Entry>& cities, const Entry& from)
    {
        std::vector<Entry> left = std::move(cities);
        cities.clear();
        cities.reserve(left.size());
        int x2 = 2 * from.cellX + from.sizeX;
        int z2 = 2 * from.cellZ + from.sizeZ;
        if (const auto self = std::ranges::find(left, from.key, &Entry::key); self != left.end()) {
            cities.push_back(std::move(*self));
            left.erase(self);
        }
        while (!left.empty()) {
            const auto next = std::ranges::min_element(left, [&](const Entry& a, const Entry& b) {
                const int64_t da = Distance2(a, x2, z2);
                const int64_t db = Distance2(b, x2, z2);
                if (da != db) {
                    return da < db;
                }
                return a.cellZ != b.cellZ ? a.cellZ < b.cellZ : a.cellX < b.cellX;
            });
            x2 = 2 * next->cellX + next->sizeX;
            z2 = 2 * next->cellZ + next->sizeZ;
            cities.push_back(std::move(*next));
            left.erase(next);
        }
    }

    // A new run from the region's cities. Unfounded cities and cities whose render is up to
    // date (isStale false, unless force) go to skipped. The rest is ordered from the start city.
    // Without a start city (nullptr: the region view) the walk starts at the first city to render
    // in the order of `cities`, and the run ends in the region view.
    inline Run Build(const std::vector<CityInfo>& cities, const CityInfo* start, const bool force,
                     const std::function<bool(const CityInfo&)>& isStale, std::string runId, std::string startedAt)
    {
        Run run;
        run.runId = std::move(runId);
        run.startedAt = std::move(startedAt);
        run.force = force;
        run.regionView = !start;
        if (start) {
            run.start = ToEntry(*start);
        }
        for (const CityInfo& city : cities) {
            if (!city.founded) {
                run.skipped.push_back({city.key, city.name, "Not founded", 0.0});
            }
            else if (!force && !isStale(city)) {
                run.skipped.push_back({city.key, city.name, "Up to date", 0.0});
            }
            else {
                run.pending.push_back(ToEntry(city));
            }
        }
        if (start) {
            Order(run.pending, run.start);
        }
        else if (!run.pending.empty()) {
            const Entry first = run.pending.front();  // RegionIndex::Cities is sorted by (cellZ, cellX)
            Order(run.pending, first);
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
    // city (nullptr: the region view) the saved order stays.
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
            Order(run.pending, *current);
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
