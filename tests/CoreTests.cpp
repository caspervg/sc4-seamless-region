// Offline tests for the small shared helpers: Guard, the message bus and path text.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "TestRunner.hpp"
#include "core/Guard.hpp"
#include "core/Messages.hpp"
#include "utils/Logger.h"

TEST_CASE(GuardedKeepsExceptionsInside)
{
    Logger::Initialize("SC4SRTests", {}, false);
    int ran = 0;
    CHECK(Guarded("test", [&] { ++ran; }));
    CHECK(!Guarded("test", [&] {
        ++ran;
        throw std::runtime_error("boom");
    }));
    CHECK(!Guarded("test", [&] {
        ++ran;
        throw 42;
    }));
    CHECK(ran == 3);
    // A callback that fails on every frame does not fill the log.
    for (int i = 0; i < 100; ++i) {
        Guarded("test", [] { throw std::runtime_error("again"); });
    }
    CHECK(Guard::gLoggedFailures == Guard::kMaxLoggedFailures);
}

TEST_CASE(PathTextIsUtf8AndNeverThrows)
{
    // Letters that no single ANSI code page holds: path::string() throws for this path.
    const std::filesystem::path path = L"C:/R\u00e9gions/\u03a9mega \u2603 \u6771\u4eac/city.sc4";
    const std::string text = Logger::PathText(path);
    const std::u8string expected = u8"C:/R\u00e9gions/\u03a9mega \u2603 \u6771\u4eac/city.sc4";
    CHECK(text == std::string(expected.begin(), expected.end()));
    CHECK(Logger::PathText({}).empty());
}

TEST_CASE(MessageBusDispatchSubscribeAndUnsubscribeInsideAHandler)
{
    MessageBus bus;
    std::vector<uint32_t> registered;
    bus.SetRegisterFn([&](const uint32_t id) { registered.push_back(id); });

    int a = 0, b = 0, late = 0;
    uint32_t tokenA = 0;
    tokenA = bus.Subscribe(Messages::kPostCityInit, [&](cIGZMessage2*) {
        ++a;
        bus.Unsubscribe(tokenA);                                                 // itself
        bus.Subscribe(Messages::kPostCityInit, [&](cIGZMessage2*) { ++late; });  // not in this round
    });
    bus.Subscribe(Messages::kPostCityInit, [&](cIGZMessage2*) { ++b; });
    bus.Subscribe(Messages::kPreCityShutdown, [&](cIGZMessage2*) { ++b; });
    // One registration per message ID, not per subscriber.
    CHECK(registered.size() == 2);

    bus.Dispatch(Messages::kPostCityInit, nullptr);
    CHECK(a == 1 && b == 1 && late == 0);
    bus.Dispatch(Messages::kPostCityInit, nullptr);
    CHECK(a == 1 && b == 2 && late == 1);
    bus.Dispatch(Messages::kPostSave, nullptr);  // nobody listens
    CHECK(a == 1 && b == 2 && late == 1);
}
