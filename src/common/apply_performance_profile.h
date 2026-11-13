// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/settings.h"

namespace Settings {
inline void ApplyPerformanceProfile() {
    const u32 profile = values.performance_profile.GetValue();
    if (profile < 1 || profile > 3) return;
    values.cpu_accuracy.SetValue(CpuAccuracy::Auto);
    values.use_multi_core.SetValue(true);
    values.use_speed_limit.SetValue(true);
    values.speed_limit.SetValue(100);
    values.current_speed_mode.SetValue(SpeedMode::Standard);
    values.use_disk_shader_cache.SetValue(true);
    values.use_vulkan_driver_pipeline_cache.SetValue(true);
    values.use_asynchronous_gpu_emulation.SetValue(true);
    values.gpu_accuracy.SetValue(profile == 3 ? GpuAccuracy::Low : GpuAccuracy::High);
    values.use_asynchronous_shaders.SetValue(profile == 3);
    values.nx2_gpu_thread_split.SetValue(profile >= 2);
    values.vram_usage_mode.SetValue(profile == 3 ? VramUsageMode::Aggressive
                                                : VramUsageMode::Conservative);
    values.renderer_force_max_clock.SetValue(false);
    values.frame_gen.SetValue(false);
    values.frame_pacing_mode.SetValue(FramePacingMode::Target_Auto);
    values.vsync_mode.SetValue(VSyncMode::Fifo);
}
} // namespace Settings
