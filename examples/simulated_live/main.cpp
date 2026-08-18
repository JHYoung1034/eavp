#include <iostream>

#include "eavp/platform/simulated_platform.hpp"

int main() {
    eavp::SimulatedPlatform platform;
    eavp::Status status = platform.initialize();
    if (!status.ok()) {
        std::cerr << "初始化失败: " << status.message() << '\n';
        return 1;
    }

    status = platform.dispatch(eavp::StartPipelineCommand("example-start", "cli", "live0"));
    if (status.ok()) {
        status = platform.reconcile_once();
    }
    if (status.ok()) {
        status = platform.tick(100U);
    }
    if (!status.ok()) {
        std::cerr << "模拟管线失败: " << status.message() << '\n';
        return 2;
    }

    const eavp::Result<std::uint64_t> processed =
        platform.metrics().counter("media.packets.processed");
    std::cout << "pipeline=live0 state=running processed=" << processed.value() << '\n';

    status = platform.dispatch(eavp::StopPipelineCommand("example-stop", "cli", "live0"));
    if (status.ok()) {
        status = platform.reconcile_once();
    }
    return status.ok() ? 0 : 3;
}
