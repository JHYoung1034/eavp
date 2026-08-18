#ifndef EAVP_BASE_TIME_HPP_
#define EAVP_BASE_TIME_HPP_

#include <cstdint>

#include "eavp/base/result.hpp"

namespace eavp {

class TimeBase {
public:
    static Result<TimeBase> create(std::int32_t numerator, std::int32_t denominator) {
        if (denominator <= 0) {
            return Result<TimeBase>(
                Status(StatusCode::kInvalidArgument, "time base denominator must be positive"));
        }
        return Result<TimeBase>(TimeBase(numerator, denominator));
    }

    std::int32_t numerator() const { return numerator_; }
    std::int32_t denominator() const { return denominator_; }

private:
    TimeBase(std::int32_t numerator, std::int32_t denominator)
        : numerator_(numerator), denominator_(denominator) {}

    std::int32_t numerator_;
    std::int32_t denominator_;
};

}  // namespace eavp

#endif  // EAVP_BASE_TIME_HPP_

