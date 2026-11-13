#!/usr/bin/env python3
from pathlib import Path
import sys

cmake = Path("src/common/CMakeLists.txt")
if not cmake.is_file():
    sys.exit("ERROR: run from ~/DED-SWITCH-EMU")
h = Path("src/common/dkb_cpu_trace.h")
c = Path("src/common/dkb_cpu_trace.cpp")
if h.exists() or c.exists():
    sys.exit("STOP: DKB CPU trace file already exists; refusing to overwrite.")
text = cmake.read_text()
if "thread_cpu_clock.h" not in text or "thread_cpu_clock.cpp" not in text:
    sys.exit("STOP: thread_cpu_clock CMake entries missing.")
if "dkb_cpu_trace.cpp" in text or "dkb_cpu_trace.h" in text:
    sys.exit("STOP: DKB CPU trace already registered.")
anchor = "    thread_cpu_clock.h"
i = text.find(anchor) + len(anchor)
if i < len(anchor):
    sys.exit("STOP: CMake insertion point not found.")
h.write_text('// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project\n// SPDX-License-Identifier: GPL-3.0-or-later\n\n#pragma once\n\n#include <array>\n#include <chrono>\n#include <optional>\n#include <string_view>\n\n#include "common/thread_cpu_clock.h"\n\nnamespace Common::DkbCpuTrace {\nenum class Role : size_t {\n    GuestCore0, GuestCore1, GuestCore2, GuestCore3, GuestCore4, GuestCore5,\n    GuestSingleCore, GpuCommand, GpuBack, VulkanWorker, Count,\n};\ninline constexpr size_t GuestCoreCount = static_cast<size_t>(Role::GuestSingleCore);\ninline constexpr size_t RoleCount = static_cast<size_t>(Role::Count);\ninline constexpr std::array<std::string_view, RoleCount> RoleNames{\n    "guest_core_0_host", "guest_core_1_host", "guest_core_2_host", "guest_core_3_host",\n    "guest_core_4_host", "guest_core_5_host", "guest_single_core_host", "gpu_command",\n    "gpu_back", "vulkan_worker",\n};\nstruct ThreadSample {\n    u64 generation{};\n    bool active{};\n    std::optional<u64> cpu_ns;\n};\nstruct Snapshot {\n    std::chrono::steady_clock::time_point wall_time;\n    std::array<ThreadSample, RoleCount> threads;\n};\nconstexpr std::optional<u64> CpuTimeDelta(const ThreadSample& before, const ThreadSample& after) {\n    if (!before.active || !after.active || before.generation != after.generation ||\n        !before.cpu_ns || !after.cpu_ns || *after.cpu_ns < *before.cpu_ns) {\n        return std::nullopt;\n    }\n    return *after.cpu_ns - *before.cpu_ns;\n}\nSnapshot ReadSnapshot();\nclass Registration {\npublic:\n    explicit Registration(Role role);\n    ~Registration();\n    Registration(const Registration&) = delete;\n    Registration& operator=(const Registration&) = delete;\nprivate:\n    ThreadCpuClock clock;\n    Role role;\n    bool registered{};\n};\n} // namespace Common::DkbCpuTrace\n')
c.write_text('// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project\n// SPDX-License-Identifier: GPL-3.0-or-later\n\n#include <mutex>\n#include "common/dkb_cpu_trace.h"\n#include "common/dkb_frame_trace.h"\n\nnamespace Common::DkbCpuTrace {\nnamespace {\nstruct Registry {\n    std::mutex mutex;\n    std::array<const ThreadCpuClock*, RoleCount> clocks{};\n    std::array<u64, RoleCount> generations{};\n};\nRegistry& GetRegistry() {\n    static Registry registry;\n    return registry;\n}\n} // namespace\n\nRegistration::Registration(Role role_) : clock{DkbFrameTrace::Enabled()}, role{role_} {\n    if (!DkbFrameTrace::Enabled()) return;\n    auto& registry = GetRegistry();\n    const auto index = static_cast<size_t>(role);\n    std::scoped_lock lock{registry.mutex};\n    if (registry.clocks[index]) return;\n    ++registry.generations[index];\n    registry.clocks[index] = &clock;\n    registered = true;\n}\nRegistration::~Registration() {\n    if (!registered) return;\n    auto& registry = GetRegistry();\n    std::scoped_lock lock{registry.mutex};\n    registry.clocks[static_cast<size_t>(role)] = nullptr;\n}\nSnapshot ReadSnapshot() {\n    auto& registry = GetRegistry();\n    std::scoped_lock lock{registry.mutex};\n    Snapshot result{.wall_time = std::chrono::steady_clock::now(), .threads = {}};\n    for (size_t i = 0; i < RoleCount; ++i) {\n        result.threads[i] = {\n            .generation = registry.generations[i],\n            .active = registry.clocks[i] != nullptr,\n            .cpu_ns = registry.clocks[i] ? registry.clocks[i]->Read() : std::nullopt,\n        };\n    }\n    return result;\n}\n} // namespace Common::DkbCpuTrace\n')
cmake.write_text(text[:i] + "\\n    dkb_cpu_trace.cpp\\n    dkb_cpu_trace.h" + text[i:])
print("Added dkb_cpu_trace.h/.cpp and registered both in common/CMakeLists.txt")
