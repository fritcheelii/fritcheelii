// SPDX-FileCopyrightText: Copyright 2026 FritCheeLii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/fs/path_util.h"

namespace Common {

/// Lightweight, thread-safe A/B benchmark recorder for 30s frame-time + GPU stall profiling.
/// Costs only a relaxed atomic load per frame when inactive.
class BenchRecorder {
public:
    struct Sample {
        double elapsed_ms;
        double frametime_ms;
        u64 astc_us;
        u64 readback_us;
    };

    struct Summary {
        std::size_t total_frames = 0;
        double elapsed_sec = 0.0;
        double avg_fps = 0.0;
        double low_1_fps = 0.0;
        double low_01_fps = 0.0;
        double avg_ft_ms = 0.0;
        double max_ft_ms = 0.0;
        std::size_t frames_over_33ms = 0;
        std::size_t frames_over_60ms = 0;
        double total_astc_ms = 0.0;
        u64 astc_calls = 0;
        double total_readback_ms = 0.0;
        u64 readback_calls = 0;
        std::string profile_tag;
        std::string csv_path;
    };

    static BenchRecorder& Instance() noexcept {
        static BenchRecorder instance;
        return instance;
    }

    [[nodiscard]] bool IsActive() const noexcept {
        return active.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double ElapsedSec() const noexcept {
        if (!IsActive()) {
            return 0.0;
        }
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_time).count();
    }

    [[nodiscard]] double RemainingSec() const noexcept {
        const double rem = target_duration_sec - ElapsedSec();
        return rem > 0.0 ? rem : 0.0;
    }

    void Start(double duration_sec, std::string profile, std::filesystem::path out_dir) {
        std::scoped_lock lock{mutex};
        samples.clear();
        samples.reserve(static_cast<std::size_t>(duration_sec * 120.0));
        has_completed = false;
        completed_summary = {};
        target_duration_sec = duration_sec > 1.0 ? duration_sec : 30.0;
        profile_tag = std::move(profile);
        output_dir = std::move(out_dir);
        frame_astc_us.store(0, std::memory_order_relaxed);
        frame_astc_calls.store(0, std::memory_order_relaxed);
        frame_readback_us.store(0, std::memory_order_relaxed);
        frame_readback_calls.store(0, std::memory_order_relaxed);
        total_astc_calls_run = 0;
        total_readback_calls_run = 0;
        start_time = std::chrono::steady_clock::now();
        active.store(true, std::memory_order_release);
    }

    void AddAstcUs(u64 us) noexcept {
        if (!IsActive()) {
            return;
        }
        frame_astc_us.fetch_add(us, std::memory_order_relaxed);
        frame_astc_calls.fetch_add(1, std::memory_order_relaxed);
    }

    void AddReadbackUs(u64 us) noexcept {
        if (!IsActive()) {
            return;
        }
        frame_readback_us.fetch_add(us, std::memory_order_relaxed);
        frame_readback_calls.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordFrame(double frametime_ms) {
        if (!active.load(std::memory_order_acquire)) {
            return;
        }
        const u64 astc_us = frame_astc_us.exchange(0, std::memory_order_relaxed);
        const u64 astc_cnt = frame_astc_calls.exchange(0, std::memory_order_relaxed);
        const u64 rb_us = frame_readback_us.exchange(0, std::memory_order_relaxed);
        const u64 rb_cnt = frame_readback_calls.exchange(0, std::memory_order_relaxed);

        std::scoped_lock lock{mutex};
        if (!active.load(std::memory_order_relaxed)) {
            return;
        }
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time)
                .count();
        samples.push_back(Sample{
            .elapsed_ms = elapsed_ms,
            .frametime_ms = frametime_ms,
            .astc_us = astc_us,
            .readback_us = rb_us,
        });
        total_astc_calls_run += astc_cnt;
        total_readback_calls_run += rb_cnt;

        if (elapsed_ms >= target_duration_sec * 1000.0) {
            FinishLocked(elapsed_ms / 1000.0);
        }
    }

    void StopNow() {
        std::scoped_lock lock{mutex};
        if (!active.load(std::memory_order_relaxed)) {
            return;
        }
        const double elapsed_sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        FinishLocked(elapsed_sec);
    }

    bool TakeCompletedSummary(Summary& out) {
        std::scoped_lock lock{mutex};
        if (!has_completed) {
            return false;
        }
        out = completed_summary;
        has_completed = false;
        return true;
    }

private:
    BenchRecorder() = default;

    void FinishLocked(double elapsed_sec) {
        active.store(false, std::memory_order_release);

        Summary s;
        s.total_frames = samples.size();
        s.elapsed_sec = elapsed_sec;
        s.profile_tag = profile_tag;
        s.astc_calls = total_astc_calls_run;
        s.readback_calls = total_readback_calls_run;

        if (!samples.empty()) {
            double sum_ft = 0.0;
            u64 sum_astc_us = 0;
            u64 sum_rb_us = 0;
            std::vector<double> sorted_ft;
            sorted_ft.reserve(samples.size());

            for (const auto& smp : samples) {
                sum_ft += smp.frametime_ms;
                if (smp.frametime_ms > s.max_ft_ms) {
                    s.max_ft_ms = smp.frametime_ms;
                }
                if (smp.frametime_ms > 33.333) {
                    ++s.frames_over_33ms;
                }
                if (smp.frametime_ms > 60.0) {
                    ++s.frames_over_60ms;
                }
                sum_astc_us += smp.astc_us;
                sum_rb_us += smp.readback_us;
                sorted_ft.push_back(smp.frametime_ms);
            }

            s.avg_ft_ms = sum_ft / static_cast<double>(samples.size());
            s.avg_fps = s.avg_ft_ms > 0.001 ? 1000.0 / s.avg_ft_ms : 0.0;
            s.total_astc_ms = static_cast<double>(sum_astc_us) / 1000.0;
            s.total_readback_ms = static_cast<double>(sum_rb_us) / 1000.0;

            std::sort(sorted_ft.begin(), sorted_ft.end(), std::greater<double>());
            const auto calc_low = [&](std::size_t divisor) {
                const std::size_t count = (std::max<std::size_t>)(1, sorted_ft.size() / divisor);
                double sum = 0.0;
                for (std::size_t i = 0; i < count; ++i) {
                    sum += sorted_ft[i];
                }
                const double avg = sum / static_cast<double>(count);
                return avg > 0.001 ? 1000.0 / avg : 0.0;
            };
            s.low_1_fps = calc_low(100);
            s.low_01_fps = calc_low(1000);
        }

        std::error_code ec;
        std::filesystem::create_directories(output_dir, ec);
        const std::time_t now_t = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &now_t);
#else
        localtime_r(&now_t, &tm_buf);
#endif
        std::ostringstream fname;
        fname << "bench_" << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ".csv";
        const auto csv_file = output_dir / fname.str();
        s.csv_path = Common::FS::PathToUTF8String(csv_file);

        std::ofstream out(csv_file, std::ios::out | std::ios::trunc);
        if (out.is_open()) {
            out << std::fixed << std::setprecision(3);
            out << "# profile=" << s.profile_tag << "\n";
            out << "# frames=" << s.total_frames << " elapsed_sec=" << s.elapsed_sec
                << " avg_fps=" << s.avg_fps << " low_1_fps=" << s.low_1_fps
                << " low_01_fps=" << s.low_01_fps << " max_ft_ms=" << s.max_ft_ms
                << " over_33ms=" << s.frames_over_33ms << " over_60ms=" << s.frames_over_60ms
                << " astc_ms=" << s.total_astc_ms << " astc_calls=" << s.astc_calls
                << " readback_ms=" << s.total_readback_ms << " readback_calls=" << s.readback_calls
                << "\n";
            out << "frame,elapsed_ms,frametime_ms,astc_us,readback_us\n";
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const auto& smp = samples[i];
                out << (i + 1) << ',' << smp.elapsed_ms << ',' << smp.frametime_ms << ','
                    << smp.astc_us << ',' << smp.readback_us << '\n';
            }
        }

        completed_summary = std::move(s);
        has_completed = true;
    }

    std::atomic<bool> active{false};
    std::chrono::steady_clock::time_point start_time{};
    double target_duration_sec = 30.0;
    std::string profile_tag;
    std::filesystem::path output_dir;

    std::atomic<u64> frame_astc_us{0};
    std::atomic<u64> frame_astc_calls{0};
    std::atomic<u64> frame_readback_us{0};
    std::atomic<u64> frame_readback_calls{0};

    std::mutex mutex;
    std::vector<Sample> samples;
    u64 total_astc_calls_run = 0;
    u64 total_readback_calls_run = 0;
    Summary completed_summary;
    bool has_completed = false;
};

} // namespace Common
