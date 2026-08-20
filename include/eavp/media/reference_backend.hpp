#ifndef EAVP_MEDIA_REFERENCE_BACKEND_HPP_
#define EAVP_MEDIA_REFERENCE_BACKEND_HPP_

#include <cstddef>
#include <memory>

#include "eavp/media/backend.hpp"

namespace eavp {

struct ReferenceBackendOptions {
    ReferenceBackendOptions()
        : queue_capacity(4U),
          output_delay(0U),
          device_lost_after_submissions(0U) {}

    std::size_t queue_capacity;
    std::size_t output_delay;
    std::size_t device_lost_after_submissions;
};

std::shared_ptr<MediaBackendProvider> create_reference_backend(
    const ReferenceBackendOptions& options) noexcept;

}  // namespace eavp

#endif  // EAVP_MEDIA_REFERENCE_BACKEND_HPP_
