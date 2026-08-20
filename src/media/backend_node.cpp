#include "eavp/media/backend_node.hpp"

#include <new>

namespace eavp {

namespace {

Status allocation_failure() {
    return Status(StatusCode::kResourceExhausted,
                  "backend node could not allocate result metadata");
}

Status unexpected_failure() {
    return Status(StatusCode::kInternal,
                  "backend node caught an unexpected backend exception");
}

bool is_temporarily_empty(const Status& status) {
    return status.code() == StatusCode::kWouldBlock ||
           status.code() == StatusCode::kNotFound;
}

}  // namespace

VideoProcessorNode::VideoProcessorNode(
    const std::string& id, std::unique_ptr<VideoProcessor> processor,
    const VideoProcessorConfig& config, std::size_t input_capacity)
    : MediaNode(id), processor_(std::move(processor)), config_(config),
      input_("frame_input", input_capacity, OverflowPolicy::kBlock),
      output_("frame_output"), drain_started_(false) {}

InputPort<VideoFrame>& VideoProcessorNode::input() { return input_; }
OutputPort<VideoFrame>& VideoProcessorNode::output() { return output_; }

Status VideoProcessorNode::on_prepare() {
    if (!processor_) {
        return Status(StatusCode::kInvalidState,
                      "video processor node has no backend instance");
    }
    try {
        return processor_->configure(config_);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status VideoProcessorNode::send_pending_output() {
    if (!pending_output_) {
        return Status::ok_status();
    }
    const Status status = output_.send(pending_output_);
    if (status.ok()) {
        pending_output_.reset();
    }
    return status;
}

Status VideoProcessorNode::on_tick() {
    try {
        Status status = send_pending_output();
        if (!status.ok()) {
            return status;
        }

        Result<std::shared_ptr<const VideoFrame> > output = processor_->receive();
        if (output.ok()) {
            pending_output_ = output.take_value();
            status = send_pending_output();
            if (!status.ok()) {
                return status;
            }
        } else {
            if (output.status().code() == StatusCode::kEndOfStream) {
                return output.status();
            }
            if (!is_temporarily_empty(output.status())) {
                return output.status();
            }
        }

        if (pending_input_) {
            status = processor_->submit(pending_input_);
            if (status.ok()) {
                pending_input_.reset();
            }
            return status;
        }

        Result<std::shared_ptr<const VideoFrame> > input = input_.receive();
        if (!input.ok()) {
            return input.status().code() == StatusCode::kNotFound
                       ? Status::ok_status()
                       : input.status();
        }
        status = processor_->submit(input.value());
        if (status.code() == StatusCode::kWouldBlock) {
            pending_input_ = input.take_value();
        }
        return status;
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status VideoProcessorNode::on_stop() {
    try {
        Status status = send_pending_output();
        if (!status.ok()) {
            return status;
        }

        Result<std::shared_ptr<const VideoFrame> > output = processor_->receive();
        if (output.ok()) {
            pending_output_ = output.take_value();
            status = send_pending_output();
            return status.ok() ? Status(StatusCode::kWouldBlock) : status;
        }
        if (!is_temporarily_empty(output.status()) &&
            output.status().code() != StatusCode::kEndOfStream) {
            return output.status();
        }
        if (output.status().code() == StatusCode::kEndOfStream) {
            return output.status();
        }

        if (!drain_started_) {
            if (pending_input_) {
                status = processor_->submit(pending_input_);
                if (!status.ok()) {
                    return status;
                }
                pending_input_.reset();
                return Status(StatusCode::kWouldBlock);
            }
            Result<std::shared_ptr<const VideoFrame> > input = input_.receive();
            if (input.ok()) {
                status = processor_->submit(input.value());
                if (status.code() == StatusCode::kWouldBlock) {
                    pending_input_ = input.take_value();
                }
                return status.ok() ? Status(StatusCode::kWouldBlock) : status;
            }
            if (input.status().code() != StatusCode::kNotFound) {
                return input.status();
            }
            status = processor_->begin_drain();
            if (!status.ok()) {
                return status;
            }
            drain_started_ = true;
        }

        output = processor_->receive();
        if (!output.ok()) {
            return output.status();
        }
        pending_output_ = output.take_value();
        status = send_pending_output();
        return status.ok() ? Status(StatusCode::kWouldBlock) : status;
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status VideoProcessorNode::on_reset() {
    pending_input_.reset();
    pending_output_.reset();
    drain_started_ = false;
    if (!processor_) {
        return Status::ok_status();
    }
    try {
        return processor_->reset();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

VideoEncoderNode::VideoEncoderNode(
    const std::string& id, std::unique_ptr<VideoEncoder> encoder,
    const VideoFormat& input_format, const VideoEncoderConfig& config,
    std::size_t input_capacity)
    : MediaNode(id), encoder_(std::move(encoder)),
      input_format_(input_format), config_(config),
      input_("frame_input", input_capacity, OverflowPolicy::kBlock),
      output_("packet_output"), drain_started_(false) {}

InputPort<VideoFrame>& VideoEncoderNode::input() { return input_; }
OutputPort<MediaPacket>& VideoEncoderNode::output() { return output_; }

Status VideoEncoderNode::on_prepare() {
    if (!encoder_) {
        return Status(StatusCode::kInvalidState,
                      "video encoder node has no backend instance");
    }
    try {
        return encoder_->configure(input_format_, config_);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status VideoEncoderNode::send_pending_output() {
    if (!pending_output_) {
        return Status::ok_status();
    }
    const Status status = output_.send(pending_output_);
    if (status.ok()) {
        pending_output_.reset();
    }
    return status;
}

Status VideoEncoderNode::on_tick() {
    try {
        Status status = send_pending_output();
        if (!status.ok()) {
            return status;
        }

        Result<std::shared_ptr<const MediaPacket> > output = encoder_->receive();
        if (output.ok()) {
            pending_output_ = output.take_value();
            status = send_pending_output();
            if (!status.ok()) {
                return status;
            }
        } else {
            if (output.status().code() == StatusCode::kEndOfStream) {
                return output.status();
            }
            if (!is_temporarily_empty(output.status())) {
                return output.status();
            }
        }

        if (pending_input_) {
            status = encoder_->submit(pending_input_);
            if (status.ok()) {
                pending_input_.reset();
            }
            return status;
        }

        Result<std::shared_ptr<const VideoFrame> > input = input_.receive();
        if (!input.ok()) {
            return input.status().code() == StatusCode::kNotFound
                       ? Status::ok_status()
                       : input.status();
        }
        status = encoder_->submit(input.value());
        if (status.code() == StatusCode::kWouldBlock) {
            pending_input_ = input.take_value();
        }
        return status;
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status VideoEncoderNode::on_stop() {
    try {
        Status status = send_pending_output();
        if (!status.ok()) {
            return status;
        }

        Result<std::shared_ptr<const MediaPacket> > output = encoder_->receive();
        if (output.ok()) {
            pending_output_ = output.take_value();
            status = send_pending_output();
            return status.ok() ? Status(StatusCode::kWouldBlock) : status;
        }
        if (!is_temporarily_empty(output.status()) &&
            output.status().code() != StatusCode::kEndOfStream) {
            return output.status();
        }
        if (output.status().code() == StatusCode::kEndOfStream) {
            return output.status();
        }

        if (!drain_started_) {
            if (pending_input_) {
                status = encoder_->submit(pending_input_);
                if (!status.ok()) {
                    return status;
                }
                pending_input_.reset();
                return Status(StatusCode::kWouldBlock);
            }
            Result<std::shared_ptr<const VideoFrame> > input = input_.receive();
            if (input.ok()) {
                status = encoder_->submit(input.value());
                if (status.code() == StatusCode::kWouldBlock) {
                    pending_input_ = input.take_value();
                }
                return status.ok() ? Status(StatusCode::kWouldBlock) : status;
            }
            if (input.status().code() != StatusCode::kNotFound) {
                return input.status();
            }
            status = encoder_->begin_drain();
            if (!status.ok()) {
                return status;
            }
            drain_started_ = true;
        }

        output = encoder_->receive();
        if (!output.ok()) {
            return output.status();
        }
        pending_output_ = output.take_value();
        status = send_pending_output();
        return status.ok() ? Status(StatusCode::kWouldBlock) : status;
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status VideoEncoderNode::on_reset() {
    pending_input_.reset();
    pending_output_.reset();
    drain_started_ = false;
    if (!encoder_) {
        return Status::ok_status();
    }
    try {
        return encoder_->reset();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

}  // namespace eavp
