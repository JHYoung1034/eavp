#include <gtest/gtest.h>

#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"

namespace {

TEST(MetricRegistryTest, CounterAccumulatesAndGaugeKeepsLatestValue) {
    eavp::MetricRegistry metrics;
    ASSERT_TRUE(metrics.increment_counter("media.packets", 40U).ok());
    ASSERT_TRUE(metrics.increment_counter("media.packets", 60U).ok());
    ASSERT_TRUE(metrics.set_gauge("pipeline.queue.depth", 3.0).ok());
    ASSERT_TRUE(metrics.set_gauge("pipeline.queue.depth", 1.0).ok());

    EXPECT_EQ(100U, metrics.counter("media.packets").value());
    EXPECT_DOUBLE_EQ(1.0, metrics.gauge("pipeline.queue.depth").value());
    EXPECT_EQ(eavp::StatusCode::kNotFound, metrics.counter("missing").status().code());
}

TEST(HealthManagerTest, AggregateTracksWorstComponentAndRecovers) {
    eavp::HealthManager health;
    ASSERT_TRUE(health.report("pipeline", eavp::HealthStatus::kOk, "running").ok());
    ASSERT_TRUE(health.report("network", eavp::HealthStatus::kError, "disconnected").ok());

    EXPECT_EQ(eavp::HealthStatus::kError, health.aggregate());
    EXPECT_EQ("disconnected", health.component("network").value().message);

    ASSERT_TRUE(health.report("network", eavp::HealthStatus::kOk, "connected").ok());
    EXPECT_EQ(eavp::HealthStatus::kOk, health.aggregate());
}

}  // namespace
