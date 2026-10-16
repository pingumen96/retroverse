#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

#include <rv/ui/timing_samples.hpp>

using Catch::Approx;
using rv::ui::percentileOfSorted;
using rv::ui::TimingSamples;
using rv::ui::TimingSummary;

TEST_CASE("ui/timing_samples: percentiles use the nearest rank", "[ui][timing_samples]") {
    std::vector<double> hundred;
    for (int value = 1; value <= 100; ++value) {
        hundred.push_back(value);
    }
    CHECK(percentileOfSorted(hundred, 50.0) == 50.0);
    CHECK(percentileOfSorted(hundred, 99.0) == 99.0);
    CHECK(percentileOfSorted(hundred, 100.0) == 100.0);
    CHECK(percentileOfSorted(hundred, 0.0) == 1.0);
    CHECK(percentileOfSorted(hundred, 250.0) == 100.0); // clamped

    const std::vector<double> four{1.0, 2.0, 3.0, 10.0};
    CHECK(percentileOfSorted(four, 50.0) == 2.0);
    CHECK(percentileOfSorted(four, 99.0) == 10.0); // one slow frame in four is the 99th percentile
    CHECK(percentileOfSorted({}, 50.0) == 0.0);
}

TEST_CASE("ui/timing_samples: the ring keeps the latest samples and ignores invalid ones", "[ui][timing_samples]") {
    TimingSamples samples(4);
    CHECK(samples.summarize().count == 0);
    samples.add(5.0);
    samples.add(-1.0);
    samples.add(std::numeric_limits<double>::quiet_NaN());
    samples.add(std::numeric_limits<double>::infinity());
    CHECK(samples.size() == 1);
    CHECK(samples.total() == 1);

    for (const double value : {1.0, 2.0, 3.0, 4.0}) {
        samples.add(value); // 5.0 is overwritten by the fourth
    }
    CHECK(samples.size() == 4);
    CHECK(samples.total() == 5);
    const TimingSummary summary = samples.summarize();
    CHECK(summary.count == 4);
    CHECK(summary.min == 1.0);
    CHECK(summary.max == 4.0);
    CHECK(summary.mean == Approx(2.5));
    CHECK(summary.p50 == 2.0);
    CHECK(summary.p99 == 4.0);

    samples.clear();
    CHECK(samples.size() == 0);
    CHECK(samples.total() == 0);
    CHECK(TimingSamples(0).capacity() == 1);
}
