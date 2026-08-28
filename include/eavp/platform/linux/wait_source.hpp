#ifndef EAVP_PLATFORM_LINUX_WAIT_SOURCE_HPP_
#define EAVP_PLATFORM_LINUX_WAIT_SOURCE_HPP_

#include <poll.h>
#include <vector>

#include "eavp/base/result.hpp"

namespace eavp {

class LinuxWaitSource {
public:
    virtual ~LinuxWaitSource() {}

    virtual Result<std::vector<struct pollfd> > poll_descriptors() = 0;
    virtual Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) = 0;
};

}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_WAIT_SOURCE_HPP_
