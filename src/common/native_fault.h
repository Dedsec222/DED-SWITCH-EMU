// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <signal.h>
namespace Common {
struct NativeFaultRecord {
    static constexpr uint64_t Magic = 0x4445444641554c54ULL;
    static constexpr size_t Slots = 32;
    uint64_t magic{}, sequence{}, completed_sequence{}, timestamp_ns{};
    int32_t signal{}, code{};
    uint64_t address{}, pc{}, sp{}, lr{}, thread{};
    uint64_t registers[31]{};
};
void InitializeNativeFaultCapture(const std::string& directory);
void RefreshNativeFaultMappings();
uint64_t BeginNativeFault(int signal, siginfo_t* info, void* context) noexcept;
void EndNativeFault(uint64_t sequence) noexcept;
} // namespace Common
