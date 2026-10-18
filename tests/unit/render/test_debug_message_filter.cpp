#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include <rv/render/debug_message_filter.hpp>

using rv::gl::DebugMessageAction;
using rv::gl::DebugMessageFilter;
using rv::gl::DebugMessageKey;
using rv::gl::DebugSeverity;

namespace {

// KHR_debug enum values, spelled out because this GPU-free test must not include an OpenGL header.
constexpr std::uint32_t kSourceApi = 0x8246;
constexpr std::uint32_t kSourceShaderCompiler = 0x8248;
constexpr std::uint32_t kTypeError = 0x824C;
constexpr std::uint32_t kTypePerformance = 0x8250;
constexpr std::uint32_t kTypeOther = 0x8251;

constexpr DebugMessageKey kInvalidEnum{kSourceApi, kTypeError, 1280};

std::vector<DebugMessageAction> classifyRepeatedly(DebugMessageFilter& filter, const DebugMessageKey& key,
                                                   DebugSeverity severity, int times) {
    std::vector<DebugMessageAction> actions;
    for (int i = 0; i < times; ++i) {
        actions.push_back(filter.classify(key, severity));
    }
    return actions;
}

} // namespace

TEST_CASE("render/debug_filter: notifications are dropped and not counted", "[render][debug_filter]") {
    DebugMessageFilter filter;
    CHECK(filter.classify({kSourceApi, kTypeOther, 1}, DebugSeverity::Notification) == DebugMessageAction::Drop);
    CHECK(filter.classify({kSourceApi, kTypeOther, 1}, DebugSeverity::Notification) == DebugMessageAction::Drop);
    CHECK(filter.count(DebugSeverity::Notification) == 0);
    CHECK(filter.suppressed().empty());
}

TEST_CASE("render/debug_filter: ignored ids are dropped whatever their severity", "[render][debug_filter]") {
    DebugMessageFilter filter(rv::gl::kNvidiaInformationalIds);
    for (const std::uint32_t id : rv::gl::kNvidiaInformationalIds) {
        INFO("id " << id);
        CHECK(filter.classify({kSourceApi, kTypeOther, id}, DebugSeverity::Low) == DebugMessageAction::Drop);
        CHECK(filter.classify({kSourceApi, kTypePerformance, id}, DebugSeverity::High) == DebugMessageAction::Drop);
    }
    CHECK(filter.count(DebugSeverity::Notification) == 0);

    CHECK(filter.classify(kInvalidEnum, DebugSeverity::High) == DebugMessageAction::Log);
    CHECK(filter.count(DebugSeverity::High) == 1);
}

TEST_CASE("render/debug_filter: ignore lists do not need to be sorted", "[render][debug_filter]") {
    const std::array<std::uint32_t, 3> ignored{900u, 5u, 300u};
    DebugMessageFilter filter(ignored);
    for (const std::uint32_t id : ignored) {
        INFO("id " << id);
        CHECK(filter.classify({kSourceApi, kTypeError, id}, DebugSeverity::High) == DebugMessageAction::Drop);
    }
    CHECK(filter.classify({kSourceApi, kTypeError, 6}, DebugSeverity::High) == DebugMessageAction::Log);
    CHECK(filter.count(DebugSeverity::High) == 1);
}

TEST_CASE("render/debug_filter: the NVIDIA ids are the documented informational ones", "[render][debug_filter]") {
    // 131154: the pixel-path warning of every video upload (ADR-0004), dropped since I3 step 11.
    CHECK(rv::gl::kNvidiaInformationalIds ==
          std::array<std::uint32_t, 5>{131154u, 131169u, 131185u, 131204u, 131218u});
}

TEST_CASE("render/debug_filter: without an ignore list every id counts", "[render][debug_filter]") {
    DebugMessageFilter filter;
    CHECK(filter.classify({kSourceApi, kTypePerformance, 131218}, DebugSeverity::Medium) == DebugMessageAction::Log);
    CHECK(filter.count(DebugSeverity::Medium) == 1);
}

TEST_CASE("render/debug_filter: repeats are logged three times, summarized once, then suppressed",
          "[render][debug_filter]") {
    DebugMessageFilter filter;
    REQUIRE(filter.repeatLimit() == 3);
    const std::vector<DebugMessageAction> expected{
        DebugMessageAction::Log,      DebugMessageAction::Log,      DebugMessageAction::Log,
        DebugMessageAction::StartSuppressing, DebugMessageAction::Suppress, DebugMessageAction::Suppress,
    };
    CHECK(classifyRepeatedly(filter, kInvalidEnum, DebugSeverity::High, 6) == expected);
    CHECK(filter.count(DebugSeverity::High) == 6); // suppressed repeats are still counted

    const auto suppressed = filter.suppressed();
    REQUIRE(suppressed.size() == 1);
    CHECK(suppressed[0].key == kInvalidEnum);
    CHECK(suppressed[0].count == 3);
}

TEST_CASE("render/debug_filter: keys that differ in source or type are tracked separately", "[render][debug_filter]") {
    DebugMessageFilter filter;
    const DebugMessageKey apiError{kSourceApi, kTypeError, 1};
    const DebugMessageKey compilerError{kSourceShaderCompiler, kTypeError, 1};
    const DebugMessageKey apiPerformance{kSourceApi, kTypePerformance, 1};

    CHECK(classifyRepeatedly(filter, apiError, DebugSeverity::Medium, 3) ==
          std::vector<DebugMessageAction>(3, DebugMessageAction::Log));
    CHECK(filter.classify(compilerError, DebugSeverity::Medium) == DebugMessageAction::Log);
    CHECK(filter.classify(apiPerformance, DebugSeverity::Medium) == DebugMessageAction::Log);
    CHECK(filter.classify(apiError, DebugSeverity::Medium) == DebugMessageAction::StartSuppressing);
    CHECK(filter.classify(compilerError, DebugSeverity::Medium) == DebugMessageAction::Log);
    CHECK(filter.count(DebugSeverity::Medium) == 7);
}

TEST_CASE("render/debug_filter: counts accumulate from a minimum severity", "[render][debug_filter]") {
    DebugMessageFilter filter;
    (void)filter.classify({kSourceApi, kTypeOther, 1}, DebugSeverity::Low);
    (void)filter.classify({kSourceApi, kTypePerformance, 2}, DebugSeverity::Medium);
    (void)filter.classify({kSourceApi, kTypePerformance, 3}, DebugSeverity::Medium);
    (void)filter.classify({kSourceApi, kTypeError, 4}, DebugSeverity::High);
    (void)filter.classify({kSourceApi, kTypeError, 5}, DebugSeverity::High);
    (void)filter.classify({kSourceApi, kTypeError, 6}, DebugSeverity::High);
    (void)filter.classify({kSourceApi, kTypeOther, 7}, DebugSeverity::Notification);

    CHECK(filter.count(DebugSeverity::Notification) == 6);
    CHECK(filter.count(DebugSeverity::Low) == 6);
    CHECK(filter.count(DebugSeverity::Medium) == 5);
    CHECK(filter.count(DebugSeverity::High) == 3);
}

TEST_CASE("render/debug_filter: custom repeat limits, zero behaves like one", "[render][debug_filter]") {
    DebugMessageFilter one({}, 1);
    CHECK(classifyRepeatedly(one, kInvalidEnum, DebugSeverity::High, 3) ==
          std::vector<DebugMessageAction>{DebugMessageAction::Log, DebugMessageAction::StartSuppressing,
                                          DebugMessageAction::Suppress});

    DebugMessageFilter zero({}, 0);
    CHECK(zero.repeatLimit() == 1);
    CHECK(classifyRepeatedly(zero, kInvalidEnum, DebugSeverity::High, 2) ==
          std::vector<DebugMessageAction>{DebugMessageAction::Log, DebugMessageAction::StartSuppressing});

    DebugMessageFilter five({}, 5);
    const auto actions = classifyRepeatedly(five, kInvalidEnum, DebugSeverity::High, 6);
    CHECK(actions[4] == DebugMessageAction::Log);
    CHECK(actions[5] == DebugMessageAction::StartSuppressing);
}

TEST_CASE("render/debug_filter: suppressed messages are listed in key order", "[render][debug_filter]") {
    DebugMessageFilter filter;
    const DebugMessageKey compilerError{kSourceShaderCompiler, kTypeError, 1};
    const DebugMessageKey apiOther{kSourceApi, kTypeOther, 300};
    const DebugMessageKey apiError{kSourceApi, kTypeError, 7};
    const DebugMessageKey quiet{kSourceApi, kTypeError, 8}; // within the limit: never listed
    classifyRepeatedly(filter, compilerError, DebugSeverity::Low, 5);
    classifyRepeatedly(filter, apiOther, DebugSeverity::Low, 4);
    classifyRepeatedly(filter, apiError, DebugSeverity::High, 6);
    classifyRepeatedly(filter, quiet, DebugSeverity::High, 3);

    const auto suppressed = filter.suppressed();
    REQUIRE(suppressed.size() == 3);
    CHECK(suppressed[0].key == apiError);
    CHECK(suppressed[0].count == 3);
    CHECK(suppressed[1].key == apiOther);
    CHECK(suppressed[1].count == 1);
    CHECK(suppressed[2].key == compilerError);
    CHECK(suppressed[2].count == 2);
}

TEST_CASE("render/debug_filter: severity names", "[render][debug_filter]") {
    CHECK(rv::gl::toString(DebugSeverity::Notification) == "notification");
    CHECK(rv::gl::toString(DebugSeverity::Low) == "low");
    CHECK(rv::gl::toString(DebugSeverity::Medium) == "medium");
    CHECK(rv::gl::toString(DebugSeverity::High) == "high");
}
