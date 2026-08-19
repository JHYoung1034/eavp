#include <gtest/gtest.h>

#include <memory>

#include "eavp/base/result.hpp"
#include "eavp/base/strong_id.hpp"
#include "eavp/base/status.hpp"
#include "eavp/base/time.hpp"

namespace {

TEST(StatusTest, FailurePreservesCodeAndMessage) {
    const eavp::Status status(eavp::StatusCode::kInvalidArgument, "invalid width");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, status.code());
    EXPECT_EQ("invalid width", status.message());
}

TEST(StatusTest, BackendFailurePreservesPortableAndNativeContext) {
    const eavp::Status status(eavp::StatusCode::kDeviceLost, "device disappeared",
                              "reference", "receive", -19);

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, status.code());
    EXPECT_EQ("reference", status.provider_id());
    EXPECT_EQ("receive", status.operation());
    EXPECT_TRUE(status.has_native_code());
    EXPECT_EQ(-19, status.native_code());
}

TEST(ResultTest, HoldsEitherValueOrFailure) {
    const eavp::Result<int> value(42);
    const eavp::Result<int> failure(
        eavp::Status(eavp::StatusCode::kNotFound, "pipeline missing"));

    ASSERT_TRUE(value.ok());
    EXPECT_EQ(42, value.value());
    EXPECT_FALSE(failure.ok());
    EXPECT_EQ(eavp::StatusCode::kNotFound, failure.status().code());
}

TEST(ResultTest, MovesUniqueValueOutOfTheResult) {
    eavp::Result<std::unique_ptr<int> > result(
        std::unique_ptr<int>(new int(42)));
    ASSERT_TRUE(result.ok());
    std::unique_ptr<int> value = result.take_value();
    ASSERT_TRUE(value.get() != NULL);
    EXPECT_EQ(42, *value);
}

TEST(ResultTest, RejectsSuccessStatusWithoutAValue) {
    const eavp::Result<int> invalid(eavp::Status::ok_status());

    EXPECT_FALSE(invalid.ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, invalid.status().code());
}

TEST(TimeBaseTest, RejectsNonPositiveDenominator) {
    EXPECT_TRUE(eavp::TimeBase::create(1, 90000).ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::TimeBase::create(1, 0).status().code());
}

TEST(StrongIdTest, RejectsEmptyValueAndPreservesTypeSpecificValue) {
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::PipelineId::create("").status().code());
    const eavp::PipelineId pipeline = eavp::PipelineId::create("live0").value();
    const eavp::NodeId node = eavp::NodeId::create("source").value();

    EXPECT_EQ("live0", pipeline.value());
    EXPECT_EQ("source", node.value());
}

}  // namespace
