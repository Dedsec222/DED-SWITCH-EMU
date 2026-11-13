// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace VideoCommon {
constexpr size_t MobilePipelineWorkers(size_t hardware_threads, int configured,
                                       bool alien, uint64_t budget) {
    const size_t available = hardware_threads > 1 ? hardware_threads - 1 : 1;
    size_t workers = std::min(available, static_cast<size_t>(std::clamp(configured, 1, 8)));
    if (alien) {
        const size_t cpu_cap = std::clamp(hardware_threads / 2, size_t{1}, size_t{4});
        const size_t memory_cap = budget != 0 && budget <= (uint64_t{3} << 30) ? 2 : 4;
        workers = std::min({workers, cpu_cap, memory_cap});
    }
    return workers;
}

struct MobileTextureLimits {
    uint64_t expected, critical, minimum;
};
constexpr MobileTextureLimits LimitAlienTextureMemory(MobileTextureLimits limits, uint64_t budget) {
    constexpr uint64_t max_cache = uint64_t{3} << 30;
    constexpr uint64_t margin = uint64_t{512} << 20;
    const uint64_t cap = budget == 0 ? max_cache : std::min(max_cache, budget - budget / 4);
    limits.critical = std::min(limits.critical, cap);
    limits.expected = std::min(limits.expected, limits.critical - std::min(margin, limits.critical / 2));
    limits.minimum = std::min({limits.minimum, limits.expected / 2, margin});
    return limits;
}
} // namespace VideoCommon
