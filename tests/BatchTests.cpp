#include "TestRunner.hpp"
#include "batch/BatchQueue.hpp"
#include "store/TileStore.hpp"

#include <string>
#include <vector>

namespace
{
    CityInfo City(const std::string& key, const int x, const int z, const int size, const bool founded = true)
    {
        CityInfo c;
        c.key = key;
        c.name = key + " name";
        c.cellX = x;
        c.cellZ = z;
        c.sizeX = size;
        c.sizeZ = size;
        c.founded = founded;
        return c;
    }

    std::vector<std::string> Keys(const std::vector<BatchQueue::Entry>& entries)
    {
        std::vector<std::string> keys;
        for (const auto& e : entries) {
            keys.push_back(e.key);
        }
        return keys;
    }

    const std::vector<CityInfo> kRegion{
        City("a", 0, 0, 1), City("b", 1, 0, 1), City("c", 4, 0, 1), City("d", 0, 1, 1),
        City("e", 2, 2, 2), City("empty", 3, 0, 1, false),
    };
}

TEST_CASE(BatchBuildFiltersUnfoundedAndUpToDate)
{
    const auto run = BatchQueue::Build(kRegion, &kRegion[0], false, [](const CityInfo& c) { return c.key != "b"; },
                                       "id", "now");
    CHECK(run.pending.size() == 4);
    CHECK(run.skipped.size() == 2);
    CHECK(run.skipped[0].key == "b" && run.skipped[0].reason == "Up to date");
    CHECK(run.skipped[1].key == "empty" && run.skipped[1].reason == "Not founded");
    CHECK(run.start.key == "a");
    CHECK(run.state == BatchQueue::State::Running);
}

TEST_CASE(BatchBuildForceRendersUpToDate)
{
    const auto run = BatchQueue::Build(kRegion, &kRegion[0], true, [](const CityInfo&) { return false; }, "id", "now");
    CHECK(run.pending.size() == 5);
    CHECK(run.skipped.size() == 1);
    CHECK(run.force);
}

TEST_CASE(BatchOrderStartsWithStartCityThenNearest)
{
    const auto run = BatchQueue::Build(kRegion, &kRegion[0], true, [](const CityInfo&) { return true; }, "id", "now");
    // Distances in doubled cell centres. From a: b and d tie (4), b has the smaller cellZ.
    // From b: d 8, e 34, c 36. From d: e 34, c 68. Then c.
    CHECK((Keys(run.pending) == std::vector<std::string>{"a", "b", "d", "e", "c"}));
}

TEST_CASE(BatchOrderWithoutStartCityInQueue)
{
    // The start city is up to date: it is not in the queue, the walk starts next to it.
    const auto run = BatchQueue::Build(kRegion, &kRegion[2], false, [](const CityInfo& c) { return c.key != "c"; },
                                       "id", "now");
    CHECK(run.pending.front().key == "e");
    CHECK(run.pending.size() == 4);
}

TEST_CASE(BatchCompleteAndFailMoveOutOfPending)
{
    auto run = BatchQueue::Build(kRegion, &kRegion[0], true, [](const CityInfo&) { return true; }, "id", "now");
    const auto first = run.pending[0];
    const auto second = run.pending[1];
    BatchQueue::Complete(run, first, 12.5);
    BatchQueue::Fail(run, second, "broken");
    CHECK(run.pending.size() == 3);
    CHECK(run.done.size() == 1 && run.done[0].seconds == 12.5);
    CHECK(run.failed.size() == 1 && run.failed[0].reason == "broken");
    CHECK(BatchQueue::Total(run) == 5);
}

TEST_CASE(BatchResumeSkipsCityThatCrashedTwice)
{
    auto run = BatchQueue::Build(kRegion, &kRegion[0], true, [](const CityInfo&) { return true; }, "id", "now");
    CHECK(BatchQueue::BeginAttempt(run, "b") == 1);
    CHECK(BatchQueue::BeginAttempt(run, "b") == 2);
    BatchQueue::BeginAttempt(run, "c");
    // Resumed in d: b failed, c (one attempt) stays, d comes first.
    const auto d = BatchQueue::ToEntry(kRegion[3]);
    BatchQueue::PrepareResume(run, &d);
    CHECK(run.failed.size() == 1 && run.failed[0].key == "b");
    CHECK(run.pending.front().key == "d");
    CHECK(run.pending.size() == 4);
    CHECK(run.attempts["c"] == 1);
}

TEST_CASE(BatchJsonRoundTrip)
{
    auto run = BatchQueue::Build(kRegion, &kRegion[1], false, [](const CityInfo& c) { return c.key != "a"; },
                                 "20261118-120000", "2026-11-18 12:00:00");
    BatchQueue::BeginAttempt(run, run.pending[0].key);
    BatchQueue::Complete(run, run.pending[0], 61.0);
    BatchQueue::Fail(run, run.pending[0], "It could not be opened: \"quoted\"");
    run.state = BatchQueue::State::Cancelled;
    run.finishedAt = "later";
    run.addTransport = true;
    run.transport = true;
    run.maxZoom = 3;

    std::string error;
    const auto back = BatchQueue::Parse(BatchQueue::ToJson(run), &error);
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    CHECK(back->runId == run.runId);
    CHECK(back->state == BatchQueue::State::Cancelled);
    CHECK(back->force == run.force);
    CHECK(back->addTransport && back->transport && back->maxZoom == 3);
    CHECK(back->start.key == "b" && back->start.cellX == 1);
    CHECK(!back->regionView);
    CHECK(Keys(back->pending) == Keys(run.pending));
    CHECK(back->pending[0].sizeX == run.pending[0].sizeX);
    CHECK(back->done.size() == 1 && back->done[0].seconds == 61.0);
    CHECK(back->failed.size() == 1 && back->failed[0].reason == run.failed[0].reason);
    CHECK(back->skipped.size() == run.skipped.size());
    CHECK(back->attempts == run.attempts);
    CHECK(back->startedAt == run.startedAt && back->finishedAt == "later");
}

TEST_CASE(BatchJsonRejectsBadInput)
{
    std::string error;
    CHECK(!BatchQueue::Parse("not json", &error));
    CHECK(!error.empty());
    CHECK(!BatchQueue::Parse(R"({"formatVersion": 99})"));
    auto run = BatchQueue::Build(kRegion, &kRegion[0], false, [](const CityInfo&) { return true; }, "id", "now");
    std::string text = BatchQueue::ToJson(run);
    const auto at = text.find("\"running\"");
    CHECK(at != std::string::npos);
    text.replace(at, 9, "\"weird\"");
    CHECK(!BatchQueue::Parse(text));
}

TEST_CASE(BatchRegionViewRendersEveryCityFromTheFirst)
{
    // No start city: every founded city that needs a render, the walk starts at the first one.
    const auto run =
        BatchQueue::Build(kRegion, nullptr, false, [](const CityInfo& c) { return c.key != "b"; }, "id", "now");
    CHECK(run.regionView);
    CHECK(run.start.key.empty());
    // From a: d 4, then e 34 from d, then c.
    CHECK((Keys(run.pending) == std::vector<std::string>{"a", "d", "e", "c"}));
    CHECK(run.skipped.size() == 2);
}

TEST_CASE(BatchRegionViewResumeKeepsTheOrder)
{
    auto run = BatchQueue::Build(kRegion, nullptr, true, [](const CityInfo&) { return true; }, "id", "now");
    BatchQueue::BeginAttempt(run, "b");
    BatchQueue::BeginAttempt(run, "b");
    const auto before = Keys(run.pending);
    BatchQueue::PrepareResume(run, nullptr);
    CHECK(run.failed.size() == 1 && run.failed[0].key == "b");
    std::vector<std::string> expected;
    for (const auto& key : before) {
        if (key != "b") {
            expected.push_back(key);
        }
    }
    CHECK(Keys(run.pending) == expected);
}

TEST_CASE(BatchJsonRegionViewRoundTripAndOldFiles)
{
    const auto run = BatchQueue::Build(kRegion, nullptr, true, [](const CityInfo&) { return true; }, "id", "now");
    std::string text = BatchQueue::ToJson(run);
    const auto back = BatchQueue::Parse(text);
    CHECK(back && back->regionView && back->start.key.empty());

    // A batch.json of an older version has no regionView: it started in a city.
    const auto at = text.find("\"regionView\": true,");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        text.erase(at, std::string("\"regionView\": true,").size());
    }
    const auto old = BatchQueue::Parse(text);
    CHECK(old && !old->regionView);
}

// The batch rule: a city is up to date only when its render is newer than its save and its
// meta.json has every level up to the chosen zoom.
TEST_CASE(BatchUpToDateNeedsEveryLevelUpToTheMaxZoom)
{
    CityRenderMeta meta;
    meta.saveFileTime = 1000;
    for (int r = 0; r < 4; ++r) {
        for (int z = 0; z < 4; ++z) {
            meta.levels.push_back({r, z, 0.0f, 0.0f, 1, 1});
        }
    }
    const std::optional<CityRenderMeta> upTo4 = meta;
    CHECK(CityMeta::NeedsRender(std::nullopt, 1000, 3));  // never rendered
    CHECK(!CityMeta::NeedsRender(upTo4, 1000, 3));
    CHECK(!CityMeta::NeedsRender(upTo4, 1000, 2));  // more than asked is fine
    CHECK(CityMeta::NeedsRender(upTo4, 1000, 4));   // zoom 5 is missing
    CHECK(CityMeta::NeedsRender(upTo4, 1001, 3));   // saved after the render
    // One level missing (a cancelled render): render again.
    CityRenderMeta part = meta;
    part.levels.erase(part.levels.begin() + 5);
    CHECK(CityMeta::NeedsRender(part, 1000, 3));
    CHECK(!CityMeta::HasLevelsUpTo(part, 3));
    // With zoom 5 on disk.
    for (int r = 0; r < 4; ++r) {
        meta.levels.push_back({r, 4, 0.0f, 0.0f, 1, 1});
    }
    CHECK(!CityMeta::NeedsRender(meta, 1000, 4));
    CHECK(CityMeta::HasLevelsUpTo(meta, 4));
}

// With the transport layer on, a city also needs every transport level up to the max zoom. With
// it off, transport levels do not count either way.
TEST_CASE(BatchUpToDateWithTheTransportLayer)
{
    CityRenderMeta meta;
    meta.saveFileTime = 1000;
    for (int r = 0; r < 4; ++r) {
        for (int z = 0; z < 4; ++z) {
            meta.levels.push_back({r, z, 0.0f, 0.0f, 1, 1});
        }
    }
    CHECK(!CityMeta::NeedsRender(meta, 1000, 3, false));
    CHECK(CityMeta::NeedsRender(meta, 1000, 3, true));  // no transport level yet
    for (int r = 0; r < 4; ++r) {
        for (int z = 0; z < 4; ++z) {
            meta.levels.push_back({r, z, 0.0f, 0.0f, 1, 1, 1.0f, true});
        }
    }
    CHECK(!CityMeta::NeedsRender(meta, 1000, 3, true));
    CHECK(!CityMeta::NeedsRender(meta, 1000, 3, false));
    CHECK(CityMeta::NeedsRender(meta, 1001, 3, true));  // saved after the render
    // One transport level missing (cancelled in the transport pass).
    CityRenderMeta part = meta;
    part.levels.pop_back();
    CHECK(CityMeta::NeedsRender(part, 1000, 3, true));
    CHECK(!CityMeta::NeedsRender(part, 1000, 3, false));
    // Transport levels never stand in for normal ones.
    CityRenderMeta onlyTransport = meta;
    std::erase_if(onlyTransport.levels, [](const CityRenderMeta::Level& l) { return !l.transport; });
    CHECK(CityMeta::NeedsRender(onlyTransport, 1000, 3, false));
    CHECK(CityMeta::NeedsRender(onlyTransport, 1000, 3, true));
}
