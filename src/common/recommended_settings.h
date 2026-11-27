// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "common/settings.h"

namespace Settings {

/// The PC the recommended settings are chosen for. The defaults stand for a capable desktop.
struct HostProfile {
    u64 ram_bytes{u64{32} << 30};  ///< System RAM
    u64 vram_bytes{u64{12} << 30}; ///< Video memory of the chosen GPU (its device-local heaps)
    bool amd{true};                ///< AMD GPU
    bool integrated{false};        ///< Integrated GPU (shares the system's RAM)
    bool windows{true};            ///< AMD's FSR and frame generation runtimes are Windows only
};

/// CheezyEmu's recommended settings for a PC. This table is the only place that lists them:
/// change a value here to change what "Find Best Settings" writes. The first part is upstream
/// Lichii's developer table with CheezyEmu's own defaults where they differ; the last part is
/// what depends on the PC. Personal choices (language, region, docked or handheld, backend,
/// device) stay out of it. Every entry is visited for every PC, so a backup made on one holds
/// what another may change.
template <typename Func>
void ForEachRecommendedSetting(Values& config, const HostProfile& host, Func&& func) {
    const auto entry = [&func]<typename Type, bool ranged>(SwitchableSetting<Type, ranged>& setting,
                                                           std::type_identity_t<Type> value) {
        func(setting, value);
    };
    constexpr u64 GiB = u64{1} << 30;
    // A "16 GB" card reports ~15+ GiB, "12 GB" ~11+ GiB, "8 GB" ~7+ GiB.
    const bool ultra_card = !host.integrated && host.vram_bytes >= 15 * GiB;
    const bool large_card = !host.integrated && host.vram_bytes >= 11 * GiB;
    const bool medium_card = !host.integrated && host.vram_bytes >= 7 * GiB;

    // Graphics > Advanced
    entry(config.gpu_accuracy, GpuAccuracy::Low);
    entry(config.dma_accuracy, DmaAccuracy::Default);
    entry(config.gpu_fence_behavior, GpuFenceBehavior::Default);
    entry(config.native_compute_write_mark, NativeComputeWriteMark::Auto);
    // Aggressive keeps more textures in video memory, which Windows also charges to RAM plus
    // page file: only where both are plentiful.
    entry(config.vram_usage_mode, large_card && host.ram_bytes >= 30 * GiB
                                      ? VramUsageMode::Aggressive
                                      : VramUsageMode::Conservative);
    entry(config.nvdec_emulation, NvdecEmulation::Gpu);
    entry(config.max_anisotropy, AnisotropyMode::Automatic);
    entry(config.accelerate_astc, AstcDecodeMode::Gpu);
    entry(config.frame_pacing_mode, FramePacingMode::Target_Auto);
    // BC3 takes a quarter of the video memory of uncompressed textures.
    entry(config.astc_recompression,
          large_card ? AstcRecompression::Uncompressed : AstcRecompression::Bc3);
    entry(config.sync_memory_operations, false);
    entry(config.renderer_force_max_clock, false);
    entry(config.use_disk_shader_cache, true);
    entry(config.use_vulkan_driver_pipeline_cache, true);
    // CheezyEmu's default since build 9 ("Skip only redrawn targets").
    entry(config.native_async_draws, NativeAsyncDraws::Skip);
    entry(config.native_pipeline_wait_ms, 8);
    entry(config.precache_shaders, PrecacheShaders::Always);
    entry(config.use_video_framerate, false);
    entry(config.mkw_30fps, false);
    entry(config.mkw_dynamic_fps, false);
    entry(config.use_reactive_flushing, false);
    entry(config.barrier_feedback_loops, true);
    entry(config.enable_buffer_history, false);

    // Graphics > Extras
    entry(config.skip_cpu_inner_invalidation, false);
    entry(config.fix_bloom_effects, false);
    entry(config.use_asynchronous_shaders, true);
    entry(config.gpu_unswizzle_texture_size, GpuUnswizzleSize::Large);
    entry(config.gpu_unswizzle_stream_size, GpuUnswizzle::Medium);
    entry(config.gpu_unswizzle_chunk_size, GpuUnswizzleChunk::Medium);
    entry(config.gpu_unswizzle_enabled, false);
#if defined(_WIN32) || defined(__linux__)
    // Other hosts skip the pinning.
    entry(config.pin_gpu_threads, true);
#endif
#ifndef __APPLE__
    // Extended dynamic state stays disabled on macOS: MoltenVK renders black screens with it.
    entry(config.dyna_state, ExtendedDynamicState::EDS2);
#endif
    entry(config.sample_shading, 0);
    entry(config.vertex_input_dynamic_state, true);

    // System
    entry(config.cpu_clock, CpuClock::Normal);
    entry(config.gpu_clock, GpuClock::Boost);
    entry(config.use_multi_core, true);
    entry(config.use_speed_limit, true);
    entry(config.speed_limit, 100);
    entry(config.slow_speed_limit, 50);
    entry(config.turbo_speed_limit, 200);
    entry(config.sync_core_speed, false);

    // CPU
    entry(config.cpu_accuracy, CpuAccuracy::Auto);
    entry(config.use_custom_cpu_ticks, false);

    // Graphics base defaults (optimized balance of performance & image quality)
    entry(config.resolution_setup, ResolutionSetup::Res1X);
    entry(config.vsync_mode, VSyncMode::Mailbox);
    entry(config.async_presentation, true);
    entry(config.scaling_filter, ScalingFilter::Fsr);
    entry(config.anti_aliasing, medium_card ? AntiAliasing::Smaa : AntiAliasing::None);
    entry(config.fsr_sharpening_slider, 140);
    entry(config.post_shader_enabled, false);
    entry(config.oot_anti_aliasing, OotAntiAliasing::NoSharpening);
    entry(config.sink_id, AudioEngine::Cubeb);

    // What depends on the PC: AMD FSR, frame generation and Ocarina of Time's render size.
    // AMD FSR 4 only runs on supported AMD GPUs (keep OFF on NVIDIA and <7 GB cards).
    entry(config.amd_fsr4, host.windows && host.amd && (large_card || medium_card));
    // Reset alternative upscaling methods so Find Best Settings always restores the ideal method.
    entry(config.nvidia_dlss, false);
    entry(config.game_dlss_network, false);
    // AMD frame generation keeps about 1.5 GB of RAM for itself: a PC with 16 GB runs out with
    // it (build 11 reports of error 1455 and whole-PC freezes), and it needs a card that holds
    // the game's frame rate.
    const bool frame_generation =
        host.windows && host.amd && medium_card && host.ram_bytes >= 24 * GiB;
    entry(config.frame_gen, frame_generation);
    entry(config.frame_gen_type,
          host.windows && host.amd ? FrameGenType::AmdFsr : config.frame_gen_type.GetValue(true));
    // 3 real-world FPS-oriented tiers:
    //  - Entry (<7 GB VRAM, e.g. GTX 1650 4 GB): Performance (50%) + 1440p docked + AA Off
    //  - Mid   (8-10 GB VRAM, e.g. RTX 3060/4060, RX 6600/7600): Quality (67%) + 1440p docked
    //  - High  (12 GB+ VRAM): Native (100% game scale) on 12 GB, Native AA + 4K docked on 16 GB+
    entry(config.oot_fsr_preset, ultra_card    ? OotFsrPreset::NativeAa
                                 : large_card  ? OotFsrPreset::Native
                                 : medium_card ? OotFsrPreset::Quality
                                               : OotFsrPreset::Performance);
    // Docked output: 4K only on 16 GB+ cards so 12 GB cards keep high FPS at 1440p.
    entry(config.oot_docked_output,
          ultra_card ? OotDockedOutput::Output4k : OotDockedOutput::Output1440p);
}

/// The table for the default PC (a capable desktop with an AMD card).
template <typename Func>
void ForEachRecommendedSetting(Values& config, Func&& func) {
    ForEachRecommendedSetting(config, HostProfile{}, std::forward<Func>(func));
}

/// A global setting that changed, with its values as BasicSetting::ToString gives them.
struct RecommendedSettingChange {
    const BasicSetting* setting;
    std::string previous;
    std::string current;
};

/// Writes the recommended values to the global settings and returns the ones that changed.
/// Per-game values, and whether a setting uses its per-game value, stay as they are. When
/// something changed, recommended_settings_backup takes the previous value of every recommended
/// setting it does not hold yet. A value it holds stays until it is restored, so applying twice
/// does not lose the values from before the first time.
std::vector<RecommendedSettingChange> ApplyRecommendedSettings(Values& config,
                                                               const HostProfile& host = {});

/// Puts back the global values recommended_settings_backup holds, clears it and returns the
/// settings that changed. Only settings in the recommended table are restored.
std::vector<RecommendedSettingChange> RestorePreviousSettings(Values& config);

} // namespace Settings
