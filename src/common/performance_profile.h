// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include "common/common_types.h"

namespace Settings {
// Android thermal status: latch severe throttling until the device returns to light or none.
class ThermalSpeedPolicy {
public:
    void Update(int status) {
        if (status < 0 || status > 6) return;
        if (status <= 1 || status >= 3) level.store(status, std::memory_order_relaxed);
    }
    u16 Limit(u16 requested) const {
        const int status = level.load(std::memory_order_relaxed);
        const u16 cap = status >= 5 ? 25 : status >= 4 ? 50 : status >= 3 ? 75 : 65535;
        return std::min(requested, cap);
    }
private:
    std::atomic<int> level{0};
};
inline ThermalSpeedPolicy thermal_speed_policy;
} // namespace Settings
