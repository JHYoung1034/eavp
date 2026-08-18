#include "eavp/platform/simulated_platform.hpp"

int main() {
    eavp::SimulatedPlatform platform;
    return platform.initialize().ok() ? 0 : 1;
}
