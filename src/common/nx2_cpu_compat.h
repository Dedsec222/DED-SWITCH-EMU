// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace Settings {
// This package repeatedly faults in guest SDK code under Android NCE (SIGILL).
// Custom mode retains explicit CPU selection for diagnosis and future compatibility tests.
constexpr bool NeedsNx2TranslatedCpu(std::uint64_t title, unsigned profile, bool native_cpu) {
    return native_cpu && profile >= 1 && profile <= 3 && title == 0x0400486024DEC000ULL;
}
} // namespace Settings
