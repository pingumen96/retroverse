#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

#include <rv/media/sample_stats.hpp>

using Catch::Approx;
using rv::media::percentileOfSorted;
using rv::media::SampleRecorder;
using rv::media::summarize;

TEST_CASE("media/sample_stats: percentiles use the nearest rank", "[media][sample_stats]") {
    std::vector<double> values;
    for (int value = 1; value <= 100; ++value) {
        values.push_back(static_cast<double>(value));
    }
    CHECK(percentileOfSorted(values, 0.50) == 50.0);
    CHECK(percentileOfSorted(values, 0.95) == 95.0);
    CHECK(percentileOfSorted(values, 0.99) == 99.0);
    CHECK(percentileOfSorted(values, 1.0) == 100.0);
    CHECK(percentileOfSorted(values, 0.0) == 1.0);
    CHECK(percentileOfSorted(values, 2.0) == 100.0);
    CHECK(percentileOfSorted(std::vector<double>{}, 0.5) == 0.0);
    const std::vector<double> three = {1.0, 2.0, 30.0};
    CHECK(percentileOfSorted(three, 0.99) == 30.0); // one value in a hundred above p99 needs a hundred values
}

TEST_CASE("media/sample_stats: summaries sort their input", "[media][sample_stats]") {
    const std::vector<double> values = {5.0, 1.0, 3.0, 2.0, 4.0};
    const auto summary = summarize(values);
    CHECK(summary.count == 5);
    CHECK(summary.min == 1.0);
    CHECK(summary.max == 5.0);
    CHECK(summary.mean == Approx(3.0));
    CHECK(summary.p50 == 3.0);
    CHECK(summarize(std::vector<double>{}).count == 0);
}

TEST_CASE("media/sample_stats: a recorder never grows its storage and still counts every value",
          "[media][sample_stats]") {
    SampleRecorder recorder(4);
    const std::vector<double> values = {2.0, 4.0, 6.0, 8.0, 100.0, -1.0};
    for (const double value : values) {
        recorder.record(value);
    }
    recorder.record(std::numeric_limits<double>::quiet_NaN());
    recorder.record(std::numeric_limits<double>::infinity());
    CHECK(recorder.count() == 6);
    CHECK(recorder.values().size() == 4);
    CHECK(recorder.overflow() == 2);
    const auto summary = recorder.summary();
    CHECK(summary.count == 6);
    CHECK(summary.max == 100.0);
    CHECK(summary.min == -1.0);
    CHECK(summary.mean == Approx(119.0 / 6.0));
    CHECK(summary.p50 == 4.0); // of the stored values

    recorder.clear();
    CHECK(recorder.count() == 0);
    CHECK(recorder.summary().count == 0);
}
